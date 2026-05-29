// movable.cpp: implements physics for inanimate models
#include "game.h"

extern int physsteps;

namespace game
{
    enum
    {
        BOXWEIGHT = 25,
        BARRELHEALTH = 50,
        BARRELWEIGHT = 25,
        PLATFORMWEIGHT = 1000,
        PLATFORMSPEED = 8,
        EXPLODEDELAY = 200
    };

    struct movable;
    extern vector<movable *> movables;
    static void sendmovableexplode(int idx);
    bool ismovableauthority();

    struct movable : dynent
    {
        int etype, mapmodel, health, weight, exploding, tag, dir;
        int entidx;             // entities::ents index this movable was built from (-1 if none)
        physent *stacked;
        vec stackpos;

        movable(const entity &e) :
            etype(e.type),
            mapmodel(e.attr2),
            health(e.type==BARREL ? (e.attr4 ? e.attr4 : BARRELHEALTH) : 0),
            weight(e.type==PLATFORM || e.type==ELEVATOR ? PLATFORMWEIGHT : (e.attr3 ? e.attr3 : (e.type==BARREL ? BARRELWEIGHT : BOXWEIGHT))),
            exploding(0),
            tag(e.type==PLATFORM || e.type==ELEVATOR ? e.attr3 : 0),
            dir(e.type==PLATFORM || e.type==ELEVATOR ? (e.attr4 < 0 ? -1 : 1) : 0),
            entidx(-1),
            stacked(NULL),
            stackpos(0, 0, 0)
        {
            state = CS_ALIVE;
            type = ENT_INANIMATE;
            yaw = e.attr1;
            if(e.type==PLATFORM || e.type==ELEVATOR) 
            {
                maxspeed = e.attr4 ? fabs(float(e.attr4)) : PLATFORMSPEED;
                if(tag) vel = vec(0, 0, 0);
                else if(e.type==PLATFORM) { vecfromyawpitch(yaw, 0, 1, 0, vel); vel.mul(dir*maxspeed); } 
                else vel = vec(0, 0, dir*maxspeed);
            }

            const char *mdlname = mapmodelname(e.attr2);
            if(mdlname) setbbfrommodel(this, mdlname);
        }
       
        void hitpush(int damage, const vec &dir, fpsent *actor, int gun)
        {
            if(etype!=BOX && etype!=BARREL) return;
            vec push(dir);
            push.mul(80*damage/weight);
            vel.add(push);
        }

        // Authority-side explosion: applies the local game::explode (damages nearby entities,
        // plays particles/sound/debris) AND broadcasts N_MOVABLEEXPLODE so peers can replicate
        // the visual + mark the movable dead. Non-authority clients only ever get the visual
        // half via applyremotemovableexplode -- they never reach this path because hitmovable /
        // suicidemovable / the exploding-timer arm all return early off-authority.
        void explode(dynent *at)
        {
            int idx = movables.find(this);
            state = CS_DEAD;
            exploding = 0;
            game::explode(true, (fpsent *)at, o, this, guns[GUN_BARREL].damage, GUN_BARREL);
            if(idx >= 0) sendmovableexplode(idx);
        }

        void damaged(int damage, fpsent *at, int gun = -1)
        {
            if(etype!=BARREL || state!=CS_ALIVE || exploding) return;
            health -= damage;
            if(health>0) return;
            if(gun==GUN_BARREL) exploding = lastmillis + EXPLODEDELAY;
            else explode(at);
        }

        void suicide()
        {
            state = CS_DEAD;
            if(etype==BARREL) explode(player1);
        }
    };

    vector<movable *> movables;
   
    void clearmovables()
    {
        if(movables.length())
        {
            cleardynentcache();
            movables.deletecontents();
        }
        // Used to be gated to m_dmsp || m_classicsp. We now instantiate in every mode so coop
        // can sync platforms / elevators / barrels via N_PLATFORM / N_PLATFORMSTATE.
        loopv(entities::ents)
        {
            const entity &e = *entities::ents[i];
            if(e.type!=BOX && e.type!=BARREL && e.type!=PLATFORM && e.type!=ELEVATOR) continue;
            movable *m = new movable(e);
            m->entidx = i;
            movables.add(m);
            m->o = e.o;
            entinmap(m);
            updatedynentcache(m);
        }
    }

    // Spawn / despawn / replace the movable associated with a single entity index. Called from
    // entities::editent() so adding a barrel in coop edit, or moving an existing one to a new
    // spot, instantly produces a live dynent (it doesn't have to wait for the next map load).
    //
    // Moving a barrel respawns it (state, health, exploding all reset) -- this mirrors how
    // health items respawn when an editor drags them around, and gives coop editors a quick way
    // to refill a destroyed barrel without /calclight or a full map reload.
    void respawnmovable(int entidx)
    {
        // First, drop any existing movable for this entity index (we'll rebuild from the
        // current entity attributes, which may have changed -- e.g. tag / type / position).
        loopv(movables) if(movables[i]->entidx == entidx)
        {
            cleardynentcache();
            delete movables[i];
            movables.remove(i);
            break;
        }
        if(!entities::ents.inrange(entidx)) return;
        const entity &e = *entities::ents[entidx];
        if(e.type != BOX && e.type != BARREL && e.type != PLATFORM && e.type != ELEVATOR) return;
        movable *m = new movable(e);
        m->entidx = entidx;
        movables.add(m);
        m->o = e.o;
        entinmap(m);
        updatedynentcache(m);
    }

    // Apply a platform direction change. broadcast=true when initiated locally (the cubescript
    // `platform` command, typed by a user or run from a trigger script that's already broadcast
    // its own N_TRIGGER); broadcast=false for the receive side and for the server's late-joiner
    // replay so we don't echo.
    //
    // Also: when broadcasting, we tag each affected platform's current position into an
    // N_PLATFORMSTATE message in the same packet -- the triggerer is the natural authority for
    // the platform's state at the moment of triggering, so the server's cache gets accurate
    // immediately. The periodic lowest-clientnum-only N_PLATFORMSTATE refresh (see
    // broadcastplatformstates below) then handles drift, plus the auto-moving-from-map-start
    // platforms that never get triggered by anyone.
    void triggerplatform(int tag, int newdir, bool broadcast)
    {
        newdir = max(-1, min(1, newdir));
        vector<int> affected;
        loopv(movables)
        {
            movable *m = movables[i];
            if(m->state!=CS_ALIVE || (m->etype!=PLATFORM && m->etype!=ELEVATOR) || m->tag!=tag) continue;
            if(!newdir)
            {
                if(m->tag) m->vel = vec(0, 0, 0);
                else m->vel.neg();
            }
            else
            {
                if(m->etype==PLATFORM) { vecfromyawpitch(m->yaw, 0, 1, 0, m->vel); m->vel.mul(newdir*m->dir*m->maxspeed); }
                else m->vel = vec(0, 0, newdir*m->dir*m->maxspeed);
            }
            if(broadcast) affected.add(i);
        }
        if(broadcast && player1->clientnum >= 0)
        {
            packetbuf p(8 + 16 + affected.length()*40, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_PLATFORM);
            putint(p, tag);
            putint(p, newdir);
            loopv(affected)
            {
                int idx = affected[i];
                movable *m = movables[idx];
                putint(p, N_MOVABLESTATE);
                putint(p, idx);
                ivec ipos(vec(m->o).mul(DMF));
                putint(p, ipos.x);
                putint(p, ipos.y);
                putint(p, ipos.z);
                ivec ivel(vec(m->vel).mul(DNF));
                putint(p, ivel.x);
                putint(p, ivel.y);
                putint(p, ivel.z);
            }
            sendclientpacket(p.finalize(), 1);
        }
    }
    ICOMMAND(platform, "ii", (int *tag, int *newdir), triggerplatform(*tag, *newdir, true));

    void stackmovable(movable *d, physent *o)
    {
        d->stacked = o;
        d->stackpos = o->o;
    }

    // Returns true if we're the lowest-clientnum client among the currently-connected players,
    // and therefore the elected movables authority. Deterministic + leaderless: if the current
    // authority leaves, the next-lowest clientnum takes over automatically. This client runs the
    // authoritative physics; everyone else snaps to the position/velocity broadcasts.
    bool ismovableauthority()
    {
        if(player1->clientnum < 0) return false;
        loopv(clients)
        {
            fpsent *d = clients[i];
            if(!d || d == player1) continue;
            if(d->clientnum >= 0 && d->clientnum < player1->clientnum) return false;
        }
        return true;
    }

    // Snap a movable's transform to the authority's broadcast.
    //
    // IMPORTANT: do NOT call entinmap() here. entinmap is the spawn-spot helper -- it ADDS
    // eyeheight to d->o because it expects a feet-position input (see physics.cpp). The
    // authority broadcasts m->o which is already in the dynent's center / eye frame, so a
    // second entinmap raises the receiver's movable by one eyeheight above the authority's.
    // For tall models (platforms, barrels) that puts them noticeably above where they should
    // be, which then makes their local "collide with level geometry" tests fire at the wrong
    // times -- the symptom we were seeing as platform shake.
    void applyremotemovablestate(int idx, const vec &pos, const vec &vel)
    {
        if(!movables.inrange(idx)) return;
        movable *m = movables[idx];
        if(m->state != CS_ALIVE) return;
        m->o = pos;
        m->vel = vel;
        updatedynentcache(m);
    }

    // Authority broadcasts pos+vel for every CS_ALIVE movable -- including stationary ones --
    // so peers can correct local-physics divergence at any time. We do NOT skip stationary
    // movables: if a barrel just landed on a platform on the authority side, dropping its
    // broadcast would leave a non-authority client whose local sim happened to tunnel through
    // the platform falling forever with no snap to bring it back. Bandwidth cost is bounded
    // (~ N movables * 20Hz * 30 bytes; a few KB/s for typical maps).
    static int lastmovablestatesend = 0;
    void broadcastmovablestates()
    {
        if(player1->clientnum < 0) return;
        if(!ismovableauthority()) return;
        if(lastmillis - lastmovablestatesend < 50) return;
        vector<int> moving;
        loopv(movables) if(movables[i]->state == CS_ALIVE) moving.add(i);
        lastmovablestatesend = lastmillis;
        if(moving.empty()) return;

        packetbuf p(8 + moving.length()*48, ENET_PACKET_FLAG_RELIABLE);
        loopv(moving)
        {
            int idx = moving[i];
            movable *m = movables[idx];
            putint(p, N_MOVABLESTATE);
            putint(p, idx);
            ivec ipos(vec(m->o).mul(DMF));
            putint(p, ipos.x);
            putint(p, ipos.y);
            putint(p, ipos.z);
            ivec ivel(vec(m->vel).mul(DNF));
            putint(p, ivel.x);
            putint(p, ivel.y);
            putint(p, ivel.z);
        }
        sendclientpacket(p.finalize(), 1);
    }

    // Non-authority side of a barrel/box explosion. The authority is the only one who reduces
    // health / triggers the local explode(); we just play the visual + audio and mark the
    // movable dead so its physics stops and rendermovables() skips it.
    void applyremotemovableexplode(int idx)
    {
        if(!movables.inrange(idx)) return;
        movable *m = movables[idx];
        if(m->state != CS_ALIVE) return;
        m->state = CS_DEAD;
        m->exploding = 0;
        // local=false on game::explode -> visual + sound + debris only, no radial damage
        // (authority already applied it locally and the engine's damage path is server-side).
        // owner=player1 just provides a vector for debris-fling direction; it doesn't matter
        // for correctness.
        game::explode(false, player1, m->o, m, guns[GUN_BARREL].damage, GUN_BARREL);
        adddecal(DECAL_SCORCH, m->o, vec(0, 0, 1), guns[GUN_BARREL].exprad/2);
    }

    // Authority-side explosion: broadcast so peers can replicate the visual + mark the movable
    // dead. Sent reliably; receivers only act on the first arrival.
    static void sendmovableexplode(int idx)
    {
        if(player1->clientnum < 0) return;
        packetbuf p(16, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_MOVABLEEXPLODE);
        putint(p, idx);
        sendclientpacket(p.finalize(), 1);
    }

    // Helper used inside the authority's physics tick: emit a single-movable N_MOVABLESTATE
    // immediately, without waiting for the next 50ms periodic broadcast. We use it on platform
    // reversal (world collision) so peers learn about the direction change within network RTT
    // instead of up to 50ms later. Without this, non-authority clients see the platform jerk
    // back to authority position at every reversal.
    static void sendsinglemovablestate(int idx)
    {
        if(player1->clientnum < 0) return;
        if(!movables.inrange(idx)) return;
        movable *m = movables[idx];
        packetbuf p(32, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_MOVABLESTATE);
        putint(p, idx);
        ivec ipos(vec(m->o).mul(DMF));
        putint(p, ipos.x); putint(p, ipos.y); putint(p, ipos.z);
        ivec ivel(vec(m->vel).mul(DNF));
        putint(p, ivel.x); putint(p, ivel.y); putint(p, ivel.z);
        sendclientpacket(p.finalize(), 1);
    }

    // ONE source of truth for movable physics: the lowest-clientnum authority runs the full
    // simulation (collision, gravity, stacking, the explode timer, platform reversal on world
    // collision). Non-authority clients DO NOT simulate AND DO NOT extrapolate -- they just hold
    // the last snapped position until the next snap arrives. Extrapolating with stale velocity
    // overshoots around collisions and produces visible shake at every snap, plus makes the
    // dynent cache too unstable to ride. Snap-and-hold + event-driven broadcasts on platform
    // reversals = stable platform on non-authority that the player can actually land on.
    //
    // Shooter-side feedback still works: hitmovable's at==player1 branch sets m->vel locally
    // and broadcasts N_MOVABLESTATE, so the shooter's pushed barrel jumps to the new state
    // immediately, peers see it within RTT, and the authority picks it up + evolves the
    // post-push physics for the next periodic snap.
    void updatemovables(int curtime)
    {
        if(!curtime) return;
        if(!ismovableauthority()) return;
        loopv(movables)
        {
            movable *m = movables[i];
            if(m->state!=CS_ALIVE) continue;
            if(m->etype==PLATFORM || m->etype==ELEVATOR)
            {
                if(m->vel.iszero()) continue;
                for(int remaining = curtime; remaining>0;)
                {
                    int step = min(remaining, 20);
                    remaining -= step;
                    if(!moveplatform(m, vec(m->vel).mul(step/1000.0f)))
                    {
                        if(m->tag) { m->vel = vec(0, 0, 0); break; }
                        else m->vel.neg();
                        // Event-driven broadcast: peers learn about the reversal immediately
                        // rather than waiting up to 50ms for the next periodic snap. Without
                        // this, the next snap drags their platform back across the reversal
                        // gap visibly.
                        sendsinglemovablestate(i);
                    }
                }
            }
            else if(m->exploding && lastmillis >= m->exploding)
            {
                m->explode(m);
                adddecal(DECAL_SCORCH, m->o, vec(0, 0, 1), guns[GUN_BARREL].exprad/2);
            }
            else if(m->maymove() || (m->stacked && (m->stacked->state!=CS_ALIVE || m->stackpos != m->stacked->o)))
            {
                if(physsteps > 0) m->stacked = NULL;
                moveplayer(m, 1, true);
            }
        }
    }

    void rendermovables()
    {
        loopv(movables)
        {
            movable &m = *movables[i];
            if(m.state!=CS_ALIVE) continue;
            vec o = m.feetpos();
            const char *mdlname = mapmodelname(m.mapmodel);
            if(!mdlname) continue;
			rendermodel(NULL, mdlname, ANIM_MAPMODEL|ANIM_LOOP, o, m.yaw, 0, MDL_LIGHT | MDL_SHADOW | MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED, &m);
        }
    }
    
    // Per-event ownership: whoever caused the event is the authority. Local detection drives;
    // duplicate explosion broadcasts get deduped server-side via explodedmovables.
    void suicidemovable(movable *m)
    {
        // No gate: whichever client's local physics detected the player-fell-on-barrel
        // (typically the falling player's client) owns the event and broadcasts the explosion.
        // If two clients somehow both detect it, server dedup handles it.
        m->suicide();
    }

    // The SHOOTER is the authority for damage they cause: it's their shot, they see the
    // barrel react with zero lag, and the resulting explosion is broadcast as N_MOVABLEEXPLODE
    // from their client. Peers' shot replication still runs hit detection locally, but their
    // hitmovable returns early -- they wait for the authoritative explode broadcast + the
    // next N_MOVABLESTATE snap to catch up on the barrel's pushed velocity.
    void hitmovable(int damage, movable *m, fpsent *at, const vec &vel, int gun)
    {
        if(at != player1) return;         // not our shot -> not our problem
        m->hitpush(damage, vel, at, gun);
        m->damaged(damage, at, gun);
        // Push the post-hit state immediately so peers see the barrel react rather than
        // waiting up to 200ms for the position authority's next 5Hz refresh.
        if(m->state == CS_ALIVE && player1->clientnum >= 0)
        {
            int idx = movables.find(m);
            if(idx >= 0)
            {
                packetbuf p(32, ENET_PACKET_FLAG_RELIABLE);
                putint(p, N_MOVABLESTATE);
                putint(p, idx);
                ivec ipos(vec(m->o).mul(DMF));
                putint(p, ipos.x); putint(p, ipos.y); putint(p, ipos.z);
                ivec ivel(vec(m->vel).mul(DNF));
                putint(p, ivel.x); putint(p, ivel.y); putint(p, ivel.z);
                sendclientpacket(p.finalize(), 1);
            }
        }
    }
}

