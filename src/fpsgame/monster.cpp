// monster.h: implements AI for single player monsters, currently client only
#include "game.h"

extern int physsteps;

namespace game
{
    static vector<int> teleports;

    static const int TOTMFREQ = 14;
    static const int NUMMONSTERTYPES = 9;

    struct monstertype      // see docs for how these values modify behaviour
    {
        short gun, speed, health, freq, lag, rate, pain, loyalty, bscale, weight;
        short painsound, diesound;
        const char *name, *mdlname, *vwepname;
    };

    static const monstertype monstertypes[NUMMONSTERTYPES] =
    {
        { GUN_FIREBALL,  15, 100, 3, 0,   100, 800, 1, 10,  90, S_PAINO, S_DIE1,   "an ogro",     "ogro",       "ogro/vwep"},
        { GUN_CG,        18,  70, 2, 70,   10, 400, 2, 10,  50, S_PAINR, S_DEATHR, "a rhino",     "monster/rhino",      NULL},
        { GUN_SG,        13, 120, 1, 100, 300, 400, 4, 14, 115, S_PAINE, S_DEATHE, "ratamahatta", "monster/rat",        "monster/rat/vwep"},
        { GUN_RIFLE,     14, 200, 1, 80,  400, 300, 4, 18, 145, S_PAINS, S_DEATHS, "a slith",     "monster/slith",      "monster/slith/vwep"},
        { GUN_RL,        12, 500, 1, 0,   200, 200, 6, 24, 210, S_PAINB, S_DEATHB, "bauul",       "monster/bauul",      "monster/bauul/vwep"},
        { GUN_BITE,      24,  50, 3, 0,   100, 400, 1, 15,  75, S_PAINP, S_PIGGR2, "a hellpig",   "monster/hellpig",    NULL},
        { GUN_ICEBALL,   11, 250, 1, 0,    10, 400, 6, 18, 160, S_PAINH, S_DEATHH, "a knight",    "monster/knight",     "monster/knight/vwep"},
        { GUN_SLIMEBALL, 15, 100, 1, 0,   200, 400, 2, 10,  60, S_PAIND, S_DEATHD, "a goblin",    "monster/goblin",     "monster/goblin/vwep"},
        { GUN_GL,        22,  50, 1, 0,   200, 400, 1, 10,  40, S_PAIND, S_DEATHD, "a spider",    "monster/spider",      NULL },
    };

    VAR(skill, 1, 3, 10);
    VAR(killsendsp, 0, 1, 1);

    bool monsterhurt;
    vec monsterhurtpos;
    
    struct monster : fpsent
    {
        int monsterstate;                   // one of M_*, M_NONE means human
    
        int mtype, tag;                     // see monstertypes table
        fpsent *enemy;                      // monster wants to kill this entity
        float targetyaw;                    // monster wants to look in this direction
        int trigger;                        // millis at which transition to another monsterstate takes place
        vec attacktarget;                   // delayed attacks
        int anger;                          // how many times already hit by fellow monster
        physent *stacked;
        vec stackpos;
    
        monster(int _type, int _yaw, int _tag, int _state, int _trigger, int _move) :
            monsterstate(_state), tag(_tag),
            stacked(NULL),
            stackpos(0, 0, 0)
        {
            type = ENT_AI;
            respawn();
            if(_type>=NUMMONSTERTYPES || _type < 0)
            {
                conoutf(CON_WARN, "warning: unknown monster in spawn: %d", _type);
                _type = 0;
            }
            mtype = _type;
            const monstertype &t = monstertypes[mtype];
            eyeheight = 8.0f;
            aboveeye = 7.0f;
            radius *= t.bscale/10.0f;
            xradius = yradius = radius;
            eyeheight *= t.bscale/10.0f;
            aboveeye *= t.bscale/10.0f;
            weight = t.weight;
            if(_state!=M_SLEEP) spawnplayer(this);
            trigger = lastmillis+_trigger;
            targetyaw = yaw = (float)_yaw;
            move = _move;
            enemy = player1;
            gunselect = t.gun;
            maxspeed = (float)t.speed*4;
            health = t.health;
            armour = 0;
            loopi(NUMGUNS) ammo[i] = 10000;
            pitch = 0;
            roll = 0;
            state = CS_ALIVE;
            anger = 0;
            copystring(name, t.name);
        }
       
        void normalize_yaw(float angle)
        {
            while(yaw<angle-180.0f) yaw += 360.0f;
            while(yaw>angle+180.0f) yaw -= 360.0f;
        }
 
        // monster AI is sequenced using transitions: they are in a particular state where
        // they execute a particular behaviour until the trigger time is hit, and then they
        // reevaluate their situation based on the current state, the environment etc., and
        // transition to the next state. Transition timeframes are parametrized by difficulty
        // level (skill), faster transitions means quicker decision making means tougher AI.

        void transition(int _state, int _moving, int n, int r) // n = at skill 0, n/2 = at skill 10, r = added random factor
        {
            monsterstate = _state;
            move = _moving;
            n = n*130/100;
            trigger = lastmillis+n-skill*(n/16)+rnd(r+1);
        }

        void monsteraction(int curtime)           // main AI thinking routine, called every frame for every monster
        {
            if(enemy->state==CS_DEAD) { enemy = player1; anger = 0; }
            normalize_yaw(targetyaw);
            if(targetyaw>yaw)             // slowly turn monster towards his target
            {
                yaw += curtime*0.5f;
                if(targetyaw<yaw) yaw = targetyaw;
            }
            else
            {
                yaw -= curtime*0.5f;
                if(targetyaw>yaw) yaw = targetyaw;
            }
            float dist = enemy->o.dist(o);
            if(monsterstate!=M_SLEEP) pitch = dist > 0 ? asin((enemy->o.z - o.z) / dist) / RAD : 0;

            if(blocked)                                                              // special case: if we run into scenery
            {
                blocked = false;
                if(!rnd(20000/monstertypes[mtype].speed))                            // try to jump over obstackle (rare)
                {
                    jumping = true;
                }
                else if(trigger<lastmillis && (monsterstate!=M_HOME || !rnd(5)))  // search for a way around (common)
                {
                    targetyaw += 90+rnd(180);                                        // patented "random walk" AI pathfinding (tm) ;)
                    transition(M_SEARCH, 1, 100, 1000);
                }
            }
            
            float enemyyaw = -atan2(enemy->o.x - o.x, enemy->o.y - o.y)/RAD;
            
            switch(monsterstate)
            {
                case M_PAIN:
                case M_ATTACKING:
                case M_SEARCH:
                    if(trigger<lastmillis) transition(M_HOME, 1, 100, 200);
                    break;
                    
                case M_SLEEP:                       // state classic sp monster start in, wait for visual contact
                {
                    if(editmode) break;          
                    normalize_yaw(enemyyaw);
                    float angle = (float)fabs(enemyyaw-yaw);
                    if(dist<32                   // the better the angle to the player, the further the monster can see/hear
                    ||(dist<64 && angle<135)
                    ||(dist<128 && angle<90)
                    ||(dist<256 && angle<45)
                    || angle<10
                    || (monsterhurt && o.dist(monsterhurtpos)<128))
                    {
                        vec target;
                        if(raycubelos(o, enemy->o, target))
                        {
                            transition(M_HOME, 1, 500, 200);
                            playsound(S_GRUNT1+rnd(2), &o);
                        }
                    }
                    break;
                }
                
                case M_AIMING:                      // this state is the delay between wanting to shoot and actually firing
                    if(trigger<lastmillis)
                    {
                        lastaction = 0;
                        attacking = true;
                        shoot(this, attacktarget);
                        transition(M_ATTACKING, 0, 600, 0);
                    }
                    break;

                case M_HOME:                        // monster has visual contact, heads straight for player and may want to shoot at any time
                    targetyaw = enemyyaw;
                    if(trigger<lastmillis)
                    {
                        vec target;
                        if(!raycubelos(o, enemy->o, target))    // no visual contact anymore, let monster get as close as possible then search for player
                        {
                            transition(M_HOME, 1, 800, 500);
                        }
                        else 
                        {
                            bool melee = false, longrange = false;
                            switch(monstertypes[mtype].gun)
                            {
                                case GUN_BITE:
                                case GUN_FIST: melee = true; break;
                                case GUN_RIFLE: longrange = true; break;
                            }
                            // the closer the monster is the more likely he wants to shoot, 
                            if((!melee || dist<20) && !rnd(longrange ? (int)dist/12+1 : min((int)dist/12+1,6)) && enemy->state==CS_ALIVE)      // get ready to fire
                            { 
                                attacktarget = target;
                                transition(M_AIMING, 0, monstertypes[mtype].lag, 10);
                            }
                            else                                                        // track player some more
                            {
                                transition(M_HOME, 1, monstertypes[mtype].rate, 0);
                            }
                        }
                    }
                    break;
                    
            }

            if(move || maymove() || (stacked && (stacked->state!=CS_ALIVE || stackpos != stacked->o)))
            {
                vec pos = feetpos();
                loopv(teleports) // equivalent of player entity touch, but only teleports are used
                {
                    entity &e = *entities::ents[teleports[i]];
                    float dist = e.o.dist(pos);
                    if(dist<16) entities::teleport(teleports[i], this);
                }

                if(physsteps > 0) stacked = NULL;
                moveplayer(this, 1, true);        // use physics to move monster
            }
        }

        void monsterpain(int damage, fpsent *d)
        {
            if(d->type==ENT_AI)     // a monster hit us
            {
                if(this!=d)            // guard for RL guys shooting themselves :)
                {
                    anger++;     // don't attack straight away, first get angry
                    int _anger = d->type==ENT_AI && mtype==((monster *)d)->mtype ? anger/2 : anger;
                    if(_anger>=monstertypes[mtype].loyalty) enemy = d;     // monster infight if very angry
                }
            }
            else if(d->type==ENT_PLAYER) // player hit us
            {
                anger = 0;
                enemy = d;
                monsterhurt = true;
                monsterhurtpos = o;
            }
            damageeffect(damage, this);
            if((health -= damage)<=0)
            {
                state = CS_DEAD;
                lastpain = lastmillis;
                playsound(monstertypes[mtype].diesound, &o);
                monsterkilled();
                gibeffect(max(-health, 0), vel, this);

                defformatstring(id, "monster_dead_%d", tag);
                execident(id);
            }
            else
            {
                transition(M_PAIN, 0, monstertypes[mtype].pain, 200);      // in this state monster won't attack
                playsound(monstertypes[mtype].painsound, &o);
            }
        }
    };

    void stackmonster(monster *d, physent *o)
    {
        d->stacked = o;
        d->stackpos = o->o;
    }

    int nummonsters(int tag, int state)
    {
        int n = 0;
        loopv(monsters) if(monsters[i]->tag==tag && (monsters[i]->state==CS_ALIVE ? state!=1 : state>=1)) n++;
        return n;
    }
    ICOMMAND(nummonsters, "ii", (int *tag, int *state), intret(nummonsters(*tag, *state)));

    void preloadmonsters()
    {
        loopi(NUMMONSTERTYPES) preloadmodel(monstertypes[i].mdlname);
        for(int i = S_GRUNT1; i <= S_SLIMEBALL; i++) preloadsound(i);
        if(m_dmsp) preloadsound(S_V_FIGHT);
        if(m_classicsp) preloadsound(S_V_RESPAWNPOINT);
    }

    vector<monster *> monsters;
    
    int nextmonster, spawnremain, numkilled, monstertotal, mtimestart, remain;
    
    void spawnmonster()     // spawn a random monster according to freq distribution in DMSP
    {
        int n = rnd(TOTMFREQ), type;
        for(int i = 0; ; i++) if((n -= monstertypes[i].freq)<0) { type = i; break; }
        monsters.add(new monster(type, rnd(360), 0, M_SEARCH, 1000, 1));
    }

    void clearmonsters()     // called after map start or when toggling edit mode to reset/spawn all monsters to initial state
    {
        removetrackedparticles();
        removetrackeddynlights();
        loopv(monsters) delete monsters[i];
        cleardynentcache();
        monsters.shrink(0);
        numkilled = 0;
        monstertotal = 0;
        spawnremain = 0;
        remain = 0;
        monsterhurt = false;
        if(m_dmsp)
        {
            nextmonster = mtimestart = lastmillis+10000;
            monstertotal = spawnremain = skill*10;
        }
        else
        {
            // Map-placed monsters from MONSTER entities. Was gated to m_classicsp; we now run
            // this in every non-DMSP mode so coop on SP maps gets its monsters. PvP maps don't
            // have MONSTER entities, so this loop is a no-op there. Each client iterates the
            // entity list in identical order so the resulting `monsters` vector indices match
            // across peers without any negotiation.
            mtimestart = lastmillis;
            loopv(entities::ents)
            {
                extentity &e = *entities::ents[i];
                if(e.type!=MONSTER) continue;
                monster *m = new monster(e.attr2, e.attr1, e.attr3, M_SLEEP, 100, 0);
                monsters.add(m);
                m->o = e.o;
                entinmap(m);
                updatedynentcache(m);
                monstertotal++;
            }
        }
        // Teleports: ungating too, same reasoning -- monsters that hit a teleport need it
        // in MP modes the same as in SP.
        teleports.setsize(0);
        loopv(entities::ents) if(entities::ents[i]->type==TELEPORT) teleports.add(i);
    }

    void endsp(bool allkilled)
    {
        conoutf(CON_GAMEINFO, allkilled ? "\f2you have cleared the map!" : "\f2you reached the exit!");
        monstertotal = 0;
        forceintermission();
    }
    ICOMMAND(endsp, "", (), endsp(false));

    
    void monsterkilled()
    {
        numkilled++;
        player1->frags = numkilled;
        remain = monstertotal-numkilled;
        if(remain>0 && remain<=5) conoutf(CON_GAMEINFO, "\f2only %d monster(s) remaining", remain);
    }

    // Per-monster ownership policy. Returns the clientnum of the client that owns monster idx
     // -- the one that ticks its AI + physics and broadcasts its position. Default policy is
     // round-robin across all connected clientnums sorted ascending; with one player this is
     // simply that player. Owner changes implicitly when someone joins/leaves because the
     // round-robin recomputes against the current clients list each tick. Local hot-swap
     // mid-action is fine: the new owner's first tick reads the monster's current state and
     // continues from there.
    static int monsterowner(int idx)
    {
        if(player1->clientnum < 0) return -1;       // not connected -> single-player; we own everything
        static vector<int> nums;
        nums.setsize(0);
        nums.add(player1->clientnum);
        loopv(clients) if(clients[i] && clients[i] != player1 && clients[i]->clientnum >= 0)
            nums.add(clients[i]->clientnum);
        if(nums.empty()) return -1;
        nums.sort();
        return nums[idx % nums.length()];
    }

    static inline bool ownsmonster(int idx)
    {
        int o = monsterowner(idx);
        return o < 0 || o == player1->clientnum;    // -1 == single-player => we own everything
    }

    // -- Coop networking ---------------------------------------------------------------------
    // 10Hz position broadcast, one packet per owned monster. Fixed-size message keeps the
    // server relay trivial (sendf with the exclude-sender suffix). Unreliable channel 0
    // because positions are continuous -- a lost packet is replaced within 100ms.
    static int lastmonsterpossent = 0;
    void broadcastmonsterpos()
    {
        if(player1->clientnum < 0) return;
        if(lastmillis - lastmonsterpossent < 100) return;
        lastmonsterpossent = lastmillis;
        loopv(monsters)
        {
            if(monsters[i]->state != CS_ALIVE || !ownsmonster(i)) continue;
            monster *m = monsters[i];
            packetbuf p(32, 0);                     // unreliable; channel 0
            putint(p, N_MONSTERPOS);
            putint(p, i);
            putint(p, int(m->o.x * DMF));
            putint(p, int(m->o.y * DMF));
            putint(p, int(m->o.z * DMF));
            putint(p, int(m->yaw));
            putint(p, int(m->pitch));
            putint(p, m->monsterstate);
            putint(p, m->move);
            sendclientpacket(p.finalize(), 0);
        }
    }

    // Receive side: snap our local monster to the owner's broadcast. Non-owners do no AI/
    // physics, so the snap is the visual. (Smooth interpolation between snapshots is a
    // separate phase on top of this.)
    void parsemonsterpos(ucharbuf &p)
    {
        int idx = getint(p);
        vec o;
        o.x = getint(p) / DMF;
        o.y = getint(p) / DMF;
        o.z = getint(p) / DMF;
        int yaw = getint(p), pitch = getint(p);
        int mstate = getint(p);
        int mv = getint(p);
        if(!monsters.inrange(idx)) return;
        monster *m = monsters[idx];
        if(m->state != CS_ALIVE) return;
        if(ownsmonster(idx)) return;                // it's ours; ignore the server's relay back
        m->o = o; m->yaw = float(yaw); m->pitch = float(pitch);
        m->monsterstate = mstate;
        m->move = mv;
        updatedynentcache(m);
    }

    void updatemonsters(int curtime)
    {
        // Random DMSP spawning only fires on the lowest-clientnum (which is owner of idx 0).
        // We'll broadcast spawned monsters via N_MONSTERSPAWN in a later phase; for now the
        // single-authority gate keeps the random-spawn rng from rolling on every client.
        if(m_dmsp && spawnremain && lastmillis>nextmonster && ownsmonster(0))
        {
            if(spawnremain--==monstertotal) { conoutf(CON_GAMEINFO, "\f2The invasion has begun!"); playsound(S_V_FIGHT); }
            nextmonster = lastmillis+1000;
            spawnmonster();
        }

        if(killsendsp && monstertotal && !spawnremain && numkilled==monstertotal) endsp(true);

        bool monsterwashurt = monsterhurt;

        loopv(monsters)
        {
            if(!ownsmonster(i)) continue;       // someone else ticks this monster's AI + physics
            if(monsters[i]->state==CS_ALIVE) monsters[i]->monsteraction(curtime);
            else if(monsters[i]->state==CS_DEAD)
            {
                if(lastmillis-monsters[i]->lastpain<2000)
                {
                    //monsters[i]->move = 0;
                    monsters[i]->move = monsters[i]->strafe = 0;
                    moveplayer(monsters[i], 1, true);
                }
            }
        }

        if(monsterwashurt) monsterhurt = false;
    }

    void rendermonsters()
    {
        loopv(monsters)
        {
            monster &m = *monsters[i];
            if(m.state!=CS_DEAD || lastmillis-m.lastpain<10000)
            {
                modelattach vwep[2];
                vwep[0] = modelattach("tag_weapon", monstertypes[m.mtype].vwepname, ANIM_VWEP_IDLE|ANIM_LOOP, 0);
                float fade = 1;
                if(m.state==CS_DEAD) fade -= clamp(float(lastmillis - (m.lastpain + 9000))/1000, 0.0f, 1.0f);
                renderclient(&m, monstertypes[m.mtype].mdlname, vwep, 0, m.monsterstate==M_ATTACKING ? -ANIM_ATTACK1 : 0, 300, m.lastaction, m.lastpain, fade);
            }
        }
    }

    static fpsent *getclientorplayer(int cn)
    {
        if(cn == player1->clientnum) return player1;
        return getclient(cn);
    }

    // Authority side: the monster's owner applies the actual HP drop. May kill, in which
    // case we broadcast N_MONSTERDIED so peers stop ticking / rendering this monster.
    static void applymonsterdamage(int idx, int damage, fpsent *attacker)
    {
        if(!monsters.inrange(idx)) return;
        monster *m = monsters[idx];
        if(m->state != CS_ALIVE) return;
        int prevstate = m->state;
        m->monsterpain(damage, attacker);     // existing path: HP, pain transition or kill
        if(prevstate == CS_ALIVE && m->state == CS_DEAD && player1->clientnum >= 0)
        {
            packetbuf p(16, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_MONSTERDIED);
            putint(p, idx);
            putint(p, attacker ? attacker->clientnum : -1);
            sendclientpacket(p.finalize(), 1);
        }
    }

    void parsemonsterhit(int idx, int damage, int attackercn)
    {
        if(!monsters.inrange(idx)) return;
        monster *m = monsters[idx];
        if(m->state != CS_ALIVE) return;
        if(ownsmonster(idx))
        {
            // Owner: full pain handling (HP + state + animation + possible kill).
            fpsent *attacker = getclientorplayer(attackercn);
            if(!attacker) attacker = player1;
            applymonsterdamage(idx, damage, attacker);
        }
        else
        {
            // Non-owner: just play the visual / audio so it feels like a hit landed.
            damageeffect(damage, m);
            playsound(monstertypes[m->mtype].painsound, &m->o);
        }
    }

    void parsemonsterdied(int idx, int attackercn)
    {
        if(!monsters.inrange(idx)) return;
        monster *m = monsters[idx];
        if(m->state != CS_ALIVE) return;
        // Mark dead; mimic the tail of monsterpain()'s death branch but skip the kill broadcast
        // (we ARE the broadcast receivers). monsterkilled() updates everyone's local kill count.
        m->state = CS_DEAD;
        m->lastpain = lastmillis;
        playsound(monstertypes[m->mtype].diesound, &m->o);
        monsterkilled();
        // gib effect uses health magnitude; we don't have it on receivers, use a constant.
        gibeffect(50, m->vel, m);
        defformatstring(id, "monster_dead_%d", m->tag);
        execident(id);
    }

    void suicidemonster(monster *m)
    {
        // SP fallthrough: existing behaviour. In MP, if we don't own the monster we route the
        // suicide through the same N_MONSTERHIT path so the owner applies it.
        int idx = monsters.find(m);
        if(idx >= 0 && !ownsmonster(idx) && player1->clientnum >= 0)
        {
            packetbuf p(16, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_MONSTERHIT);
            putint(p, idx);
            putint(p, 400);
            putint(p, player1->clientnum);
            sendclientpacket(p.finalize(), 1);
            return;
        }
        m->monsterpain(400, player1);
    }

    void hitmonster(int damage, monster *m, fpsent *at, const vec &vel, int gun)
    {
        // Per-event authority: only the shooter (at==player1) acts on a hit. Peers running
        // their own hit() in response to N_SHOTFX just return -- the shooter will send
        // N_MONSTERHIT, the owner will broadcast back any resulting state changes.
        if(at != player1) return;
        int idx = monsters.find(m);
        if(idx < 0) return;
        if(ownsmonster(idx))
        {
            // Shooter is also the owner: apply directly, no round-trip.
            applymonsterdamage(idx, damage, at);
        }
        else if(player1->clientnum >= 0)
        {
            // Shooter feedback locally (so the hit feels instant); owner gets the
            // authoritative N_MONSTERHIT and decides what actually happens to HP.
            damageeffect(damage, m);
            playsound(monstertypes[m->mtype].painsound, &m->o);
            packetbuf p(16, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_MONSTERHIT);
            putint(p, idx);
            putint(p, damage);
            putint(p, at->clientnum);
            sendclientpacket(p.finalize(), 1);
        }
        else
        {
            // Single-player / not connected -- original behaviour.
            m->monsterpain(damage, at);
        }
    }

    void spsummary(int accuracy)
    {
        conoutf(CON_GAMEINFO, "\f2--- single player time score: ---");
        int pen, score = 0;
        pen = ((lastmillis-maptime)*100)/game::scaletime(1000); score += pen; if(pen) conoutf(CON_GAMEINFO, "\f2time taken: %d seconds (%d simulated seconds)", pen, (lastmillis-maptime)/1000);
        pen = player1->deaths*60; score += pen; if(pen) conoutf(CON_GAMEINFO, "\f2time penalty for %d deaths (1 minute each): %d seconds", player1->deaths, pen);
        pen = remain*10;          score += pen; if(pen) conoutf(CON_GAMEINFO, "\f2time penalty for %d monsters remaining (10 seconds each): %d seconds", remain, pen);
        pen = (10-skill)*20;      score += pen; if(pen) conoutf(CON_GAMEINFO, "\f2time penalty for lower skill level (20 seconds each): %d seconds", pen);
        pen = 100-accuracy;       score += pen; if(pen) conoutf(CON_GAMEINFO, "\f2time penalty for missed shots (1 second each %%): %d seconds", pen);
        defformatstring(aname, "bestscore_%s", getclientmap());
        const char *bestsc = getalias(aname);
        int bestscore = *bestsc ? parseint(bestsc) : score;
        if(score<bestscore) bestscore = score;
        defformatstring(nscore, "%d", bestscore);
        alias(aname, nscore);
        conoutf(CON_GAMEINFO, "\f2TOTAL SCORE (time + time penalties): %d seconds (best so far: %d seconds)", score, bestscore);
    }
}

