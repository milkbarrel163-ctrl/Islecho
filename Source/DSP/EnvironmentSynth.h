#pragma once
#include "PulseDSP.h"
#include <array>
#include <cmath>
#include <algorithm>

// Eight permanently allocated subtractive channels, separate from animal voices.
// Coefficients update at control boundaries; the callback only advances filters,
// anti-aliased oscillators, noise and sample-rate-independent smoothers.
struct EnvironmentSynth {
    struct Channel {
        float phase=0, increment=0, nextIncrement=0, songIncrement=0, gain=0, targetGain=0;
        float coefficient=0, targetCoefficient=0, low1=0, low2=0;
        float left=0, right=0, targetLeft=0, targetRight=0;
    };
    std::array<Channel,PF_WAVE_CHANNELS> channels{};
    float smoothing=0, fade=0, enabled=0, targetEnabled=0;
    float wind=0, targetWind=0, windLow[2]{}, windHigh[2]{}, lowCoeff=0, highCoeff=0;
    double windPhase=0,windStep=0,sampleRate=48000;
    int windControl=0;
    float windCoefficient=0;
    uint32_t random=0x472fbc91;
    void reset(double sr) {
        *this={}; random=0x472fbc91;sampleRate=sr;windStep=1/(13.7*sr);
        smoothing=float(1-std::exp(-1/(sr*.08)));
        fade=float(1-std::exp(-1/(sr*.18)));
        lowCoeff=float(1-std::exp(-6.28318530718*1800/sr));
        highCoeff=float(1-std::exp(-6.28318530718*150/sr));windCoefficient=lowCoeff;
        for(int i=0;i<PF_WAVE_CHANNELS;++i) {
            channels[i].phase=float(i)/PF_WAVE_CHANNELS;
            channels[i].coefficient=channels[i].targetCoefficient=float(1-std::exp(-6.28318530718*500/sr));
        }
    }
    void set(const PFEnvironmentSound &frame,double sr) {
        targetEnabled=frame.enabled ? 1:0;
        targetWind=frame.wind;
        for(int i=0;i<PF_WAVE_CHANNELS;++i) {
            auto &c=channels[i];const auto &p=frame.waves[i];
            c.nextIncrement=std::min(p.frequency,float(sr*.2))/float(sr);
            if(c.increment==0)c.increment=c.nextIncrement;
            c.targetCoefficient=float(1-std::exp(-6.28318530718*p.cutoffHz/sr));
            c.targetGain=p.gain*.024f;
            c.targetLeft=std::sqrt((1-p.pan)*.5f);c.targetRight=std::sqrt((1+p.pan)*.5f);
        }
    }
    static float blep(float t,float dt) {
        if(dt<=0)return 0;
        if(t<dt){t/=dt;return t+t-t*t-1;}
        if(t>1-dt){t=(t-1)/dt;return t*t+t+t+1;}
        return 0;
    }
    void harmony(const std::array<int,8> &notes,double sr) {
        for(int i=0;i<8;++i)channels[i].songIncrement=440.f*std::pow(2.f,float(notes[i]-69)/12)/float(sr);
    }
    void sample(float &left,float &right,float songMix=0,double /*songBeat*/=0) {
        // Free-running 13.7-second LFO; phase survives tempo/score changes.
        windPhase+=windStep;if(windPhase>=1)windPhase-=1;
        if(windControl--<=0) {
            windControl=127;
            const double hz=1500+700*std::sin(6.28318530718*windPhase);
            windCoefficient=float(1-std::exp(-6.28318530718*hz/sampleRate));
        }
        lowCoeff+=smoothing*(windCoefficient-lowCoeff);
        enabled+=fade*(targetEnabled-enabled);
        if(enabled<1e-7f && targetEnabled==0) {enabled=0;return;}
        wind+=smoothing*(targetWind-wind);
        float l=0,r=0;
        for(auto &c:channels) {
            const float pitch=c.nextIncrement+(c.songIncrement-c.nextIncrement)*songMix;
            c.increment+=smoothing*(pitch-c.increment);
            c.coefficient+=smoothing*(c.targetCoefficient-c.coefficient);
            c.gain+=smoothing*(c.targetGain-c.gain);
            c.left+=smoothing*(c.targetLeft-c.left);c.right+=smoothing*(c.targetRight-c.right);
            float saw=2*c.phase-1-blep(c.phase,c.increment);
            c.phase+=c.increment;if(c.phase>=1)c.phase-=1;
            c.low1+=c.coefficient*(saw-c.low1);c.low2+=c.coefficient*(c.low1-c.low2);
            l+=c.low2*c.gain*c.left;r+=c.low2*c.gain*c.right;
        }
        float noise[2];
        for(int i=0;i<2;++i) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            float n=float(random&0xFFFFFF)/8388608.f-1;
            windLow[i]+=lowCoeff*(n-windLow[i]);windHigh[i]+=highCoeff*(windLow[i]-windHigh[i]);
            noise[i]=(windLow[i]-windHigh[i])*wind*.10f;
        }
        left+=enabled*(l+noise[0]);right+=enabled*(r+noise[1]);
    }
};
