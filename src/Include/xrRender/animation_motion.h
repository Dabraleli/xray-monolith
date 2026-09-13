#pragma once

// Coop-only transient snapshot. Explicit wire fields; never part of saved STATE.
struct CoopStalkerLayers
{
    u8 ready = 0;
    u16 held_id = 65535;
    u32 held_state = 0;
    float duration[5] = {};
    u8 looping = 0;
    u32 motion[5] = {65535,65535,65535,65535,65535};
    float phase[5] = {};
    float amount[5] = {};
    float speed[5] = {1,1,1,1,1};
    float rotation[4][4] = {{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
    // Let the renderer advance every frame. Correct clock drift through speed,
    // not repeated seeks to a packet phase. No gameplay callbacks on replicas.
    template<class Blend> void sync_blend(u32 i, Blend* b, bool restart) const
    {
        const float total=b->timeTotal;
        float target=_max(0.f,_min(phase[i],total));
        float error=target-b->timeCurrent;
        if ((looping & (1u<<i)) && total>EPS) {
            error=fmodf(error,total);
            if(error>total*.5f) error-=total;
            if(error<-total*.5f) error+=total;
        }
        b->playing=TRUE; b->fall_at_end=FALSE; b->blendAmount=amount[i];
        b->speed=speed[i];
        if(restart || _abs(error)>.25f || _abs(speed[i])<EPS)
            b->timeCurrent=target;
        else {
            const float limit=_abs(speed[i])*.25f;
            b->speed+=_max(-limit,_min(error*4.f,limit));
        }
    }
    template<class P> void write(P& p) const
    {
        p.w_u8(ready);
        p.w_u16(held_id); p.w_u32(held_state); p.w_u8(looping);
        for (u32 i=0;i<5;++i) p.w_float(duration[i]);
        for (u32 i=0;i<5;++i) { p.w_u32(motion[i]); p.w_float(phase[i]); p.w_float(speed[i]); p.w_float(amount[i]); }
        for (u32 i=0;i<4;++i) for(u32 j=0;j<4;++j) p.w_float_q16(rotation[i][j],-1.f,1.f);
    }
    template<class P> void read(P& p)
    {
        ready=p.r_u8();
        held_id=p.r_u16(); held_state=p.r_u32(); looping=p.r_u8();
        for (u32 i=0;i<5;++i) duration[i]=p.r_float();
        for (u32 i=0;i<5;++i) { motion[i]=p.r_u32(); phase[i]=p.r_float(); speed[i]=p.r_float(); amount[i]=p.r_float(); }
        for (u32 i=0;i<4;++i) for(u32 j=0;j<4;++j) rotation[i][j]=p.r_float_q16(-1.f,1.f);
    }
};

struct MotionID
{
private:
	typedef const MotionID*(MotionID::*unspecified_bool_type)() const;
public:
	union
	{
		struct
		{
			u16 idx:16; //14
			u16 slot:16; //2
		};

		//.		u16			val;
		u32 val;
	};

public:
	MotionID() { invalidate(); }
	MotionID(u16 motion_slot, u16 motion_idx) { set(motion_slot, motion_idx); }
	ICF bool operator==(const MotionID& tgt) const { return tgt.val == val; }
	ICF bool operator!=(const MotionID& tgt) const { return tgt.val != val; }
	ICF bool operator<(const MotionID& tgt) const { return val < tgt.val; }
	ICF bool operator!() const { return !valid(); }
	ICF void set(u16 motion_slot, u16 motion_idx)
	{
		slot = motion_slot;
		idx = motion_idx;
	}

	ICF void invalidate() { val = u16(-1); }
	ICF bool valid() const { return val != u16(-1); }
	const MotionID* get() const { return this; };
	ICF operator unspecified_bool_type() const
	{
		if (valid()) return &MotionID::get;
		else return 0;
		//		return(!valid()?0:&MotionID::get);
	}
};
