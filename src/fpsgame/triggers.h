#ifndef __TRIGGERS_H__
#define __TRIGGERS_H__

// Trigger-type lookup shared by the client-side state visualizer in entities.cpp and the
// server-side authoritative state machine in server.cpp. attr3 on a trigger mapmodel
// indexes triggertypes[]; -1 means the index is not a real trigger type.

namespace entities
{
    enum
    {
        TRIG_COLLIDE    = 1<<0,
        TRIG_TOGGLE     = 1<<1,
        TRIG_ONCE       = 0<<2,
        TRIG_MANY       = 1<<2,
        TRIG_DISAPPEAR  = 1<<3,
        TRIG_AUTO_RESET = 1<<4,
        TRIG_RUMBLE     = 1<<5,
        TRIG_LOCKED     = 1<<6,
        TRIG_ENDSP      = 1<<7
    };

    static const int NUMTRIGGERTYPES = 32;

    static const int triggertypes[NUMTRIGGERTYPES] =
    {
        -1,
        TRIG_ONCE,                    // 1
        TRIG_RUMBLE,                  // 2
        TRIG_TOGGLE,                  // 3
        TRIG_TOGGLE | TRIG_RUMBLE,    // 4
        TRIG_MANY,                    // 5
        TRIG_MANY | TRIG_RUMBLE,      // 6
        TRIG_MANY | TRIG_TOGGLE,      // 7
        TRIG_MANY | TRIG_TOGGLE | TRIG_RUMBLE,    // 8
        TRIG_COLLIDE | TRIG_TOGGLE | TRIG_RUMBLE, // 9
        TRIG_COLLIDE | TRIG_TOGGLE | TRIG_AUTO_RESET | TRIG_RUMBLE, // 10
        TRIG_COLLIDE | TRIG_TOGGLE | TRIG_LOCKED | TRIG_RUMBLE,     // 11
        TRIG_DISAPPEAR,               // 12
        TRIG_DISAPPEAR | TRIG_RUMBLE, // 13
        TRIG_DISAPPEAR | TRIG_COLLIDE | TRIG_LOCKED, // 14
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        TRIG_DISAPPEAR | TRIG_RUMBLE | TRIG_ENDSP, // 29
        -1, -1,
    };

    static inline bool validtriggertype(int type)
    {
        return triggertypes[type & (NUMTRIGGERTYPES-1)] >= 0;
    }

    static inline int triggertypeflags(int type)
    {
        return triggertypes[type & (NUMTRIGGERTYPES-1)];
    }
}

#endif
