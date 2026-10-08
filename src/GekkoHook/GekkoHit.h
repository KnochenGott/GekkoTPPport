// GekkoHit.h 30/09/2026. Hit callback for the gekko's target objects (natives of the same kind carry a TargetCallbackInterface at +0x30).
#pragma once

namespace GekkoHit
{
    void Install();          // sets the callback interface on every hit-target object of the gekko
    void SetEnabled(bool on);   // off: the hit filter refuses every hit, so bullets pass through the gekko
    void Clear();            // drops the queued records (hits that arrived while the brain was not running)
    unsigned Count();        // hits received since Install
    // One hit on one of the gekko's own target objects, as TPP delivers it (OffenseTargetData3, layout in GekkoHit.cpp): damage[] are the ratio bytes +0x2a..+0x2c (0..200, 200 = full strength, not points),
    // attackId the u16 at +0x28, flags +0x2e, owner the attacking object's game object id word.
    // pos is the bullet direction (unit vectors in the log, not a position), hit the world hit point read from the GeoResult (hasHit false when unreadable), attackerId the u16 at +0x24 (what TPP's handlers pass on as Damage +0x22).
    struct Record { int part; unsigned char damage[3]; unsigned char flags; float pos[3]; float dist; unsigned short attackId; unsigned long long owner; float hit[3]; bool hasHit; unsigned short attackerId; };   // part = index in GekkoFull::TargetObjects
    unsigned Drain(Record* out, unsigned max);   // pops up to max queued records (the newest 64 are kept)
}
