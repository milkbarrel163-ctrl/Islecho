#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// C++ translation of WorldMeshFactory.organism and OrganismAppearance. Meshes
// are generated once per variant; the ecology seed only chooses an instance.
namespace OrganismMesh {
constexpr float pi = 3.14159265358979323846f;
constexpr int variantCount = 12;
struct V {
    float x=0,y=0,z=0;
    V operator+(V b) const { return {x+b.x,y+b.y,z+b.z}; }
    V operator-(V b) const { return {x-b.x,y-b.y,z-b.z}; }
    V operator*(float s) const { return {x*s,y*s,z*s}; }
    V operator/(float s) const { return {x/s,y/s,z/s}; }
};
inline V mul(V a,V b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
inline V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
inline V unit(V a) { const float n=length(a);return n>1.e-8f?a/n:V{0,1,0}; }
struct Vertex { float position[3],normal[3],color[3],emission; };
struct Appearance { int variant; V proportions,tint;float angle; };
struct Random {
    uint32_t seed;
    float next() { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return float(seed&0xffffff)/float(0xffffff); }
};
inline Appearance appearance(uint32_t seed) {
    Random r{seed?seed:1};
    Appearance a;
    a.variant=int(r.next()*variantCount)%variantCount;
    a.proportions={.82f+r.next()*.42f,.82f+r.next()*.42f,.82f+r.next()*.42f};
    a.tint={.65f+r.next()*.35f,.72f+r.next()*.28f,.65f+r.next()*.35f};
    a.angle=r.next()*2*pi;
    return a;
}
struct Builder {
    std::vector<Vertex> vertices;
    void triangle(V a,V b,V c,V color,float emission=0) {
        const V n=unit(cross(b-a,c-a));
        for(V p:{a,b,c}) vertices.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},{color.x,color.y,color.z},emission});
    }
    void rod(V a,V b,float radius,V color,float emission=0,int sides=8) {
        const V axis=unit(b-a),r=unit(cross(axis,std::abs(axis.y)<.9f?V{0,1,0}:V{1,0,0})),s=cross(axis,r);
        for(int i=0;i<sides;++i){
            const float t=2*pi*i/sides,n=2*pi*(i+1)/sides;
            const V u=(r*std::cos(t)+s*std::sin(t))*radius,v=(r*std::cos(n)+s*std::sin(n))*radius;
            triangle(a+u,b+u*.75f,a+v,color,emission);
            triangle(a+v,b+u*.75f,b+v*.75f,color,emission);
        }
    }
    void gem(V center,V radius,V color,bool wire=false,float emission=0) {
        const V p[]={center+V{radius.x,0,0},center+V{-radius.x,0,0},center+V{0,radius.y,0},center+V{0,-radius.y,0},center+V{0,0,radius.z},center+V{0,0,-radius.z}};
        const int faces[][3]={{0,2,4},{4,2,1},{1,2,5},{5,2,0},{0,4,3},{4,1,3},{1,5,3},{5,0,3}};
        for(auto& f:faces) {
            if(wire){rod(p[f[0]],p[f[1]],.013f,color,emission);rod(p[f[1]],p[f[2]],.013f,color,emission);}
            else triangle(p[f[0]],p[f[1]],p[f[2]],color,emission);
        }
    }
    void ring(V center,float radius,float thickness,V color,bool vertical=false) {
        for(int i=0;i<36;++i){
            auto p=[&](float a,float r){return center+(vertical?V{std::cos(a)*r,std::sin(a)*r,0}:V{std::cos(a)*r,0,std::sin(a)*r});};
            const float a=2*pi*i/36,b=2*pi*(i+1)/36;
            triangle(p(a,radius-thickness),p(a,radius+thickness),p(b,radius+thickness),color,.85f);
            triangle(p(a,radius-thickness),p(b,radius+thickness),p(b,radius-thickness),color,.85f);
        }
    }
    void roundedOrb(V center,V radius,V color) {
        auto p=[&](int i,int j){const float a=2*pi*i/16,t=pi*j/10;return center+mul({std::cos(a)*std::sin(t),std::cos(t),std::sin(a)*std::sin(t)},radius);};
        for(int j=0;j<10;++j)for(int i=0;i<16;++i){triangle(p(i,j),p(i+1,j),p(i,j+1),color,.7f);triangle(p(i+1,j),p(i+1,j+1),p(i,j+1),color,.7f);}
    }
    void curvedTube(V a,V b,V c,V d,float radius,float endRadius,V color) {
        auto center=[&](float t){const float u=1-t;return a*(u*u*u)+b*(3*u*u*t)+c*(3*u*t*t)+d*(t*t*t);};
        V plane=cross(b-a,d-a);
        if(length(plane)<1.e-4f)plane=cross(c-a,d-a);
        if(length(plane)<1.e-4f)plane=cross(unit(d-a),V{0,0,1});
        const V reference=unit(plane);
        auto p=[&](float t,float v){
            const V q=center(t),tangent=unit(center(std::min(1.f,t+.002f))-center(std::max(0.f,t-.002f)));
            const V x=unit(cross(tangent,reference)),z=cross(tangent,x);
            const float cap=std::min(.16f,std::max(.025f,endRadius/std::max(.1f,length(d-a))*1.8f));
            const float end=std::max(0.f,(t-(1-cap))/cap),rounded=std::sqrt(std::max(0.f,1-end*end));
            const float r=(radius+(endRadius-radius)*t)*(1+.018f*std::sin(t*pi*5))*rounded;
            return q+(x*std::cos(v*2*pi)+z*std::sin(v*2*pi))*r;
        };
        for(int j=0;j<28;++j)for(int i=0;i<16;++i){
            const float t=float(j)/28,n=float(j+1)/28,v=float(i)/16,w=float(i+1)/16;
            triangle(p(t,v),p(n,v),p(t,w),color,.65f);
            triangle(p(n,v),p(n,w),p(t,w),color,.65f);
        }
    }
};
inline std::vector<Vertex> build(int kind,int variant) {
    Builder m;Random r{uint32_t(variant+1)*0x45d9f3bu};
    const int count=3+variant%5;
    const V cyan{.08f,.95f,.89f},lime{.8f,1,.23f},violet{.7f,.48f,1},cream{.92f,.94f,.65f};
    const V accent=variant%3==0?V{1,.20f,.48f}:variant%3==1?V{.91f,1,.14f}:V{.89f,.78f,1};
    auto link=[&](V a,V b,V color){m.rod(a,b,.015f,color,.95f,kind==1?12:8);};
    if(kind==0){
        if(variant%3==0){
            const float len=.30f+r.next()*.22f,w=.16f+r.next()*.12f;
            for(int i=0;i<count;++i){
                const float a=2*pi*i/count,b=2*pi*(i+1)/count;
                const V p{-len,std::cos(a)*w,std::sin(a)*w},q{len*.65f,std::cos(a+.28f)*w*.60f,std::sin(a+.28f)*w*.60f};
                link(p,q,cyan);link(p,{-len,std::cos(b)*w,std::sin(b)*w},cyan);
                link(q,{len*.65f,std::cos(b+.28f)*w*.60f,std::sin(b+.28f)*w*.60f},cyan);
                if(i%2==0){const V tip=p+V{-.12f,std::cos(a)*.12f,std::sin(a)*.12f};link(p,tip,accent);m.gem(tip,{.028f,.028f,.028f},accent,false,1);}
            }
        }else if(variant%3==1){
            m.ring({},.25f,.015f,cyan);m.ring({.10f,0,0},.20f,.015f,cyan,true);
            for(int i=0;i<count;++i){const float a=i*2.399f;const V tip{std::cos(a)*.42f,std::sin(a)*.17f,std::sin(a)*.32f};link({},tip,cream);m.gem(tip,{.04f,.04f,.04f},accent,false,1);}
        }else{
            const V p[]={{-.35f,-.06f,-.23f},{.31f,-.06f,-.15f},{.09f,-.02f,.27f},{-.09f,.30f,0}};
            const int edge[][2]={{0,1},{1,2},{2,0},{3,0},{3,1},{3,2}};
            for(auto& e:edge)link(p[e[0]],p[e[1]],cyan);
            for(int i=0;i<3;++i){const V tip=p[i]*1.40f;link(p[i],tip,accent);const float s=.045f+(variant%4)*.006f;m.gem(tip,{s,s,s},accent,false,1);}
            if(variant>5)m.ring({0,.08f,0},.19f,.014f,cream);
        }
        m.gem({.07f,0,0},{.06f,.08f,.05f},accent,false,1);
    }else if(kind==1){
        const float h=.63f+r.next()*.24f;
        m.curvedTube({}, {.04f,h*.3f,0},{-.03f,h*.65f,.02f},{0,h,0},.032f,.021f,lime);
        for(int i=0;i<count;++i){const float a=i*2.399f,y=.20f+float(i)/count*.43f;
            const V tip{std::cos(a)*(.15f+r.next()*.18f),y+.12f,std::sin(a)*.22f};link({0,y,0},tip,cream);
            if((i+variant)%2==0)m.ring(tip,.07f+r.next()*.07f,.016f,accent,variant%3==0);
            else {const float s=.055f+r.next()*.035f;m.roundedOrb(tip,{s,s,s},accent);}
        }
        if(variant%2==0)m.ring({0,h,0},.17f,.017f,lime);
        else m.gem({0,h,0},{.11f,.10f,.11f},cream,true,1);
    }else{
        for(int i=0;i<count;++i){const float a=2*pi*i/count;
            const V p{std::cos(a)*.24f,std::sin(a)*.24f,0},q{std::cos(a+.9f)*.19f,std::sin(a+.9f)*.19f,.24f};
            link(p,q,violet);link(p,{0,0,-.22f},violet);
            const V tip=p+V{std::cos(a)*.13f,std::sin(a)*.13f,0};link(p,tip,cream);
            const float s=.035f+r.next()*.025f;m.gem(tip,{s,s,s},accent,false,1);
        }
        m.gem({},{.07f,.07f,.07f},accent,false,1);
        if(variant%2==0)m.ring({},.31f,.013f,violet);
    }
    return std::move(m.vertices);
}
}
