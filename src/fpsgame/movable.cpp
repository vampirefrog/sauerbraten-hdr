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

    struct movable : dynent
    {
        int etype, mapmodel, health, weight, exploding, tag, dir;
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

        void explode(dynent *at)
        {
            state = CS_DEAD;
            exploding = 0;
            game::explode(true, (fpsent *)at, o, this, guns[GUN_BARREL].damage, GUN_BARREL);
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
        // can sync platforms / elevators / barrels via N_PLATFORM / N_HITMOVABLE -- positions
        // are still computed locally per-client (deterministic linear motion until something
        // hits a wall or a tag-driven stop) so no per-tick sync traffic is needed.
        loopv(entities::ents)
        {
            const entity &e = *entities::ents[i];
            if(e.type!=BOX && e.type!=BARREL && e.type!=PLATFORM && e.type!=ELEVATOR) continue;
            movable *m = new movable(e);
            movables.add(m);
            m->o = e.o;
            entinmap(m);
            updatedynentcache(m);
        }
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
                putint(p, N_PLATFORMSTATE);
                putint(p, idx);
                ivec ipos(vec(m->o).mul(DMF));
                putint(p, ipos.x);
                putint(p, ipos.y);
                putint(p, ipos.z);
                // Direction sign relative to the platform's reference axis, same convention
                // applyremoteplatformstate consumes.
                int signdir;
                if(!newdir) signdir = 0;
                else if(m->etype == PLATFORM)
                {
                    vec axis; vecfromyawpitch(m->yaw, 0, 1, 0, axis);
                    signdir = axis.dot(m->vel) >= 0 ? +1 : -1;
                }
                else signdir = m->vel.z >= 0 ? +1 : -1;
                putint(p, signdir);
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

    // Late joiners create their movables at the map default positions (clearmovables); the
    // server replays the last cached N_PLATFORMSTATE for each moving platform in its welcome
    // packet so the joiner spawns in-flight platforms at their current location, with the
    // current direction, so physics continues from there.
    void applyremoteplatformstate(int idx, const vec &pos, int dir)
    {
        if(!movables.inrange(idx)) return;
        movable *m = movables[idx];
        if(m->state != CS_ALIVE || (m->etype != PLATFORM && m->etype != ELEVATOR)) return;
        m->o = pos;
        entinmap(m);
        updatedynentcache(m);
        // Re-derive velocity from direction the way the constructor does, so the local sim
        // resumes the same trajectory rather than picking up some quantised remnant.
        int d2 = clamp(dir, -1, 1);
        if(!d2) m->vel = vec(0, 0, 0);
        else if(m->etype == PLATFORM) { vecfromyawpitch(m->yaw, 0, 1, 0, m->vel); m->vel.mul(d2 * m->dir * m->maxspeed); }
        else m->vel = vec(0, 0, d2 * m->dir * m->maxspeed);
    }

    // Returns true if we're the lowest-clientnum client among the currently-connected players,
    // and therefore the elected platform-state authority. Deterministic + leaderless: if the
    // current authority leaves, the next lowest-clientnum client takes over automatically.
    static bool isplatformstateauthority()
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

    // Periodic broadcast so the server's cache stays current and a fresh joiner gets recent
    // positions in their welcome packet. Cheap (a few ints per moving platform, once a second)
    // and only fires when there's actually a platform in motion to report. Per-message format
    // mirrors the welcome replay so the server can store and forward without parsing.
    static int lastplatformstatesend = 0;
    void broadcastplatformstates()
    {
        if(player1->clientnum < 0) return;          // not connected
        if(!isplatformstateauthority()) return;     // only the elected sender broadcasts
        if(lastmillis - lastplatformstatesend < 1000) return;
        // First collect the moving platforms; only construct + send a packet if there are any.
        vector<int> moving;
        loopv(movables)
        {
            movable *m = movables[i];
            if(m->state != CS_ALIVE) continue;
            if(m->etype != PLATFORM && m->etype != ELEVATOR) continue;
            if(m->vel.iszero()) continue;
            moving.add(i);
        }
        lastplatformstatesend = lastmillis;
        if(moving.empty()) return;

        packetbuf p(8 + moving.length()*32, ENET_PACKET_FLAG_RELIABLE);
        loopv(moving)
        {
            int idx = moving[i];
            movable *m = movables[idx];
            putint(p, N_PLATFORMSTATE);
            putint(p, idx);
            ivec ipos(vec(m->o).mul(DMF));
            putint(p, ipos.x);
            putint(p, ipos.y);
            putint(p, ipos.z);
            int signdir;
            if(m->etype == PLATFORM)
            {
                vec axis; vecfromyawpitch(m->yaw, 0, 1, 0, axis);
                signdir = axis.dot(m->vel) >= 0 ? +1 : -1;
            }
            else signdir = m->vel.z >= 0 ? +1 : -1;
            putint(p, signdir);
        }
        sendclientpacket(p.finalize(), 1);
    }

    void updatemovables(int curtime)
    {
        if(!curtime) return;
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
    
    void suicidemovable(movable *m)
    {
        m->suicide();
    }

    void hitmovable(int damage, movable *m, fpsent *at, const vec &vel, int gun)
    {
        m->hitpush(damage, vel, at, gun);
        m->damaged(damage, at, gun);
    }
}

