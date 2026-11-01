#pragma once
#include "PulseDSP.h"
#include <array>
#include <algorithm>
#include <cmath>

// Audio-owned bounded score and sample-clock transport; no UI frame timer,
// allocation or ecological RNG. Changes enter only at a four-beat boundary.
struct SongTransport {
    PFSongScore score{}, pending{};
    bool running=false, hasPending=false;
    double origin=0,end=0,nextStep=0,swapAt=0,nextHarmony=0;
    int step=0,chord=-1,cursor=0;
    float blend=0;
    std::array<int,8> harmony{{48,55,60,64,67,72,76,79}};
    void start(const PFSongScore &s,double beat) {
        if(running) { pending=s;hasPending=true;swapAt=origin+std::ceil((beat-origin+0.001)/4)*4;return; }
        score=s;running=true;hasPending=false;origin=beat+1;nextStep=origin;
        end=origin+std::ceil(s.durationSeconds*s.bpm/240)*4;
        step=cursor=0;chord=-1;nextHarmony=origin+s.lengthBeats*4;
    }
    void stop() { running=false;hasPending=false; }
    int degree(int midi) const {
        int best=0,distance=999;
        for(int i=0;i<score.scaleCount*5;++i) {
            int n=36+score.root+(i/score.scaleCount)*12+score.scale[i%score.scaleCount];
            if(std::abs(n-midi)<distance) { best=i;distance=std::abs(n-midi); }
        }
        return best;
    }
    int note(int d) const {
        d=std::max(0,std::min(score.scaleCount*5-1,d));
        return 36+score.root+(d/score.scaleCount)*12+score.scale[d%score.scaleCount];
    }
    int chordDegree(int section) const {
        constexpr int seven[]={0,5,3,4},five[]={0,3,1,4};
        return score.scaleCount==7 ? seven[section%4]:five[section%4];
    }
    int nearestChord(int midi,int section) const {
        const int root=chordDegree(section);int best=60+score.root,distance=999;
        for(int i=0;i<score.scaleCount*5;++i) {
            int pc=i%score.scaleCount;
            if(pc!=root && pc!=(root+2)%score.scaleCount && pc!=(root+4)%score.scaleCount)continue;
            int n=note(i);
            if(n<42+score.root || n>84+score.root)continue;
            if(std::abs(n-midi)<distance){best=n;distance=std::abs(n-midi);}
        }
        return best;
    }
    void updateHarmony(int section) {
        if(section==chord)return;
        for(int i=0;i<8;++i) harmony[i]=nearestChord(chord<0 ? 48+score.root+(i%4)*7:harmony[i],section);
        chord=section;
    }
    int melody(int index,int pass,int section) const {
        int original=score.notes[index].midi;
        if(pass==0)return original; // The first phrase repeats the actually tapped notes.
        int d=degree(original);
        // One learned note takes one scale step per repeat. The rest remains
        // recognizable; phrase endings settle into the current harmony.
        if(index==(pass-1)%score.count)d+=(pass%2 ? 1:-1);
        int n=note(d);
        if(index==score.count-1 && section>0)n=nearestChord(n,section);
        return n;
    }
    static float frequency(int midi) { return 440.f*std::pow(2.f,float(midi-69)/12); }
    void advanceBlend(double beat,double sr) {
        if(running && beat>=end)stop();
        float target=running ? float(std::min(1.,std::max(0.,(end-beat)/8))):0;
        float amount=float(1/(sr*(target>blend ? 1.2:2.0)));
        blend+=std::max(-amount,std::min(amount,target-blend));
    }
};
