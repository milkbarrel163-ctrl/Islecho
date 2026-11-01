#include "IslandScene.h"
#include "WaterCrests.h"
#include <BinaryData.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace {
using V = std::array<float, 3>;
constexpr float pi = 3.14159265358979323846f;
float clamp01(float x) { return std::max(0.0f, std::min(1.0f, x)); }
float smooth(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
float random01(uint32_t& seed) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return float(seed & 0xffffff) / float(0xffffff); }
// Exact WorldShaders.metal waveRandom; GLSL 1.10 has no unsigned integers, so
// crest parameters are evaluated here and uploaded as uniforms.
}

IslandScene::IslandScene() {
    context.setRenderer(this);
    context.setComponentPaintingEnabled(true); // JUCE draws controls above the OpenGL world.
    context.setMultisamplingEnabled(true);
}

IslandScene::~IslandScene() { detach(); }
void IslandScene::attach(juce::Component& component) { context.attachTo(component); }
void IslandScene::detach() { context.detach(); }
void IslandScene::resized(int w, int h) { viewWidth = w; viewHeight = h; repaint(); }
void IslandScene::repaint() { context.triggerRepaint(); }
void IslandScene::setDaylight(float value) { daylight=clamp01(value); repaint(); }
void IslandScene::setWater(float elapsedSeconds, float windAmount) {
    waterTime=elapsedSeconds; wind=clamp01(windAmount); repaint();
}
void IslandScene::setOrganisms(std::vector<Organism> value) {
    { std::lock_guard<std::mutex> lock(organismMutex); organisms=std::move(value); }
}

void IslandScene::orbit(juce::Point<float> from, juce::Point<float> to) {
    const float nextYaw=yaw.load()-(to.x-from.x)*.007f;
    yaw=std::atan2(std::sin(nextYaw),std::cos(nextYaw));
    elevation=std::clamp(elevation.load()+(to.y-from.y)*.006f,.38f,1.48f);
    repaint();
}

IslandScene::Vec3 IslandScene::focusPlanePoint(juce::Point<float> screen) const {
    const auto c=camera();
    const float dx=(screen.x-c.field.getCentreX())/c.scale;
    const float dy=(screen.y-c.field.getCentreY())/c.scale;
    const Vec3 ray{c.focus.x+c.right.x*dx-c.up.x*dy,c.focus.y+c.right.y*dx-c.up.y*dy,c.focus.z+c.right.z*dx-c.up.z*dy};
    const float t=(c.focus.y-ray.y)/c.forward.y;
    return {ray.x+c.forward.x*t,c.focus.y,ray.z+c.forward.z*t};
}

void IslandScene::navigate(juce::Point<float> oldA,juce::Point<float> oldB,
                           juce::Point<float> newA,juce::Point<float> newB) {
    const auto oldMid=(oldA+oldB)*.5f,newMid=(newA+newB)*.5f;
    const auto anchor=focusPlanePoint(oldMid);
    const float before=oldA.getDistanceFrom(oldB),after=newA.getDistanceFrom(newB);
    if(before>12&&after>12)zoom=std::clamp(zoom.load()*after/before,.65f,3.2f);
    const auto moved=focusPlanePoint(newMid);
    centerX=std::clamp(centerX.load()+anchor.x-moved.x,-16.0f,16.0f);
    centerZ=std::clamp(centerZ.load()+anchor.z-moved.z,-14.0f,14.0f);
    repaint();
}

void IslandScene::zoomAt(juce::Point<float> anchor,float factor) {
    if(!std::isfinite(factor)||factor<=0)return;
    const auto before=focusPlanePoint(anchor);
    zoom=std::clamp(zoom.load()*factor,.65f,3.2f);
    const auto after=focusPlanePoint(anchor);
    centerX=std::clamp(centerX.load()+before.x-after.x,-16.0f,16.0f);
    centerZ=std::clamp(centerZ.load()+before.z-after.z,-14.0f,14.0f);
    repaint();
}

float IslandScene::bend(float u) {
    return sceneryScale * (0.65f * std::sin(u * 2 * pi) + 0.22f * std::cos(u * 5 * pi));
}

float IslandScene::heightAt(float u, float v) { return height(u, v); }

float IslandScene::height(float u, float v) {
    if (u <= 0 || u >= 1 || v <= 0 || v >= 0.60f) return 0;
    auto mound = [=](float x, float z, float rx, float rz, float tilt = 0) {
        const float a = u - x, b = v - z - tilt * a;
        return std::exp(-a*a/(rx*rx) - b*b/(rz*rz));
    };
    const float bank = smooth((0.60f-v)/0.25f);
    const float coast = smooth(std::min(u,1-u)/0.14f) * smooth(v/0.14f);
    const float ridge = 1.64f*mound(.24f,.245f,.205f,.145f,.30f);
    const float shoulder = 1.06f*mound(.76f,.22f,.19f,.12f,-.40f);
    const float toe = .38f*mound(.84f,.405f,.15f,.10f,-.25f);
    const float spineOffset = (v-.16f-.045f*std::sin(u*7.5f))/.13f;
    const float spine = .34f*std::exp(-spineOffset*spineOffset);
    float raw = .30f+ridge+shoulder+toe+spine;
    const float channel = u-.48f-.055f*std::sin(v*10.5f+.3f);
    const float drainage = std::exp(-channel*channel/.0049f);
    const float tributary = u-.20f-.19f*v-.027f*std::sin(v*17);
    const float runnel = std::exp(-tributary*tributary/.0009f)*smooth((v-.22f)/.12f);
    raw *= 1-.48f*drainage-.16f*runnel;
    const float hollows = .18f*mound(.16f,.26f,.045f,.032f,.5f)
        +.14f*mound(.31f,.33f,.052f,.038f,-.4f)
        +.15f*mound(.76f,.22f,.042f,.035f,.5f)
        +.10f*mound(.86f,.34f,.035f,.025f,-.5f);
    const float strata = .032f*std::sin(raw*22+u*5)*smooth((v-.13f)/.13f);
    const float weathering = .024f*std::sin(u*63+v*17+.8f*std::sin(v*19))*std::sin(v*51-u*11);
    raw = raw*(1-hollows)+strata+weathering;
    return sceneryScale*bank*coast*raw/(1+raw*.12f);
}

IslandScene::Vec3 IslandScene::position(float u, float v) {
    float a=u*2-1,b=v*2-1;
    const float extent=std::max({1.0f,std::abs(a),std::abs(b)});a/=extent;b/=extent;
    const float x=a*std::sqrt(std::max(0.0f,1-b*b/2))*extent;
    const float z=b*std::sqrt(std::max(0.0f,1-a*a/2))*extent;
    return {x*width/2,height(u,v),z*depth/2+bend(x*.5f+.5f)};
}

juce::Point<double> IslandScene::coordinates(Vec3 p) {
    double x=p.x/(width/2),z=(p.z-bend(float(x*.5+.5)))/(depth/2);
    const double extent=std::max(1.0,std::hypot(x,z));x/=extent;z/=extent;
    const double s=std::sqrt(2.0),xx=2+x*x-z*z,zz=2-x*x+z*z;
    const double a=(std::sqrt(std::max(0.0,xx+2*s*x))-std::sqrt(std::max(0.0,xx-2*s*x)))/2;
    const double b=(std::sqrt(std::max(0.0,zz+2*s*z))-std::sqrt(std::max(0.0,zz-2*s*z)))/2;
    return {(a*extent+1)/2,(b*extent+1)/2};
}

IslandScene::Vec3 IslandScene::normal(float u, float v) {
    const float e=.001f;
    const auto l=position(u-e,v),r=position(u+e,v),d=position(u,v-e),f=position(u,v+e);
    const float ax=r.x-l.x,ay=r.y-l.y,az=r.z-l.z;
    const float bx=f.x-d.x,by=f.y-d.y,bz=f.z-d.z;
    float x=by*az-bz*ay,y=bz*ax-bx*az,z=bx*ay-by*ax;
    const float inv=1/std::max(.000001f,std::sqrt(x*x+y*y+z*z));
    return {x*inv,y*inv,z*inv};
}

IslandScene::Vec3 IslandScene::groundColor(Vec3 p) {
    const auto uv=coordinates(p);
    const float u=float(uv.x),v=float(uv.y);
    const float variation=.026f*std::sin(p.x*.63f+p.z*.39f)*std::sin(p.z*.52f-p.x*.16f);
    const float woodland=smooth((.46f-v-variation)/.22f)*smooth(std::min(u,1-u)/.16f)*smooth(v/.12f);
    const float shade=.88f+.10f*std::sin(p.x*1.7f+p.z*.7f)*std::sin(p.z*1.25f-p.x*.35f);
    return {mix(.090f,.052f,woodland)*shade,mix(.058f,.067f,woodland)*shade,mix(.044f,.040f,woodland)*shade};
}

float IslandScene::floorHeight(Vec3 p) {
    const auto uv=coordinates(p);
    if (uv.x>=0 && uv.x<=1 && uv.y>=0 && uv.y<=.60) return height(float(uv.x),float(uv.y));
    return 0; // The dry island is the picking surface; sea remains presentation only.
}

// LakeWaterBody.depth: the dry island, then a wide apron down to the 5.1 basin.
float IslandScene::waterDepth(Vec3 p) {
    const auto uv=coordinates(p);
    const float u=float(uv.x),v=float(uv.y);
    float floor=0;
    if(u>=0&&u<=1&&v>=0&&v<=.60f) floor=height(u,v);
    else {
        Vec3 nearest=position(std::clamp(u,0.0f,1.0f),std::clamp(v,0.0f,.60f));
        const float px=p.x/(width/2),pz=(p.z-bend(p.x/width+.5f))/(depth/2);
        const float radius=std::hypot(px,pz);
        if(radius>1){
            const float qx=px/radius,qz=pz/radius;
            const Vec3 coast{qx*width/2,0,qz*depth/2+bend(qx*.5f+.5f)};
            if(coordinates(coast).y<=.60) nearest=coast;
        }
        const float distance=std::hypot(p.x-nearest.x,p.z-nearest.z);
        const float variation=1+.035f*std::sin(p.x*.30f+p.z*.19f)*std::sin(p.z*.35f);
        const float t=clamp01(distance/(7.5f*variation));
        floor=-5.1f*t*t*t*(t*(t*6-15)+10);
    }
    return std::max(0.0f,.06f-floor);
}

IslandScene::Camera IslandScene::camera() const {
    Camera c;
    const float w=float(std::max(1,viewWidth.load())),h=float(std::max(1,viewHeight.load()));
    // iOS presents the terrarium behind the full-screen controls.  Trimming
    // the field to the old 2D panel would make the island much too small.
    c.field=juce::Rectangle<float>(0,0,w,h);
    const float angle=yaw.load(),pitch=elevation.load();
    c.right={std::cos(angle),0,-std::sin(angle)};
    c.forward={std::sin(angle)*std::cos(pitch),std::sin(pitch),std::cos(angle)*std::cos(pitch)};
    c.up={c.forward.y*c.right.z-c.forward.z*c.right.y,c.forward.z*c.right.x-c.forward.x*c.right.z,c.forward.x*c.right.y-c.forward.y*c.right.x};
    c.focus={centerX.load(),.65f,centerZ.load()-.45f};
    c.scale=std::max(.001f,std::min(c.field.getWidth()/19.2f,c.field.getHeight()/14.0f)*zoom.load());
    const float sx=2*c.scale/c.field.getWidth(),sy=2*c.scale/c.field.getHeight();
    const Vec3 r{c.right.x*sx,c.right.y*sx,c.right.z*sx};
    const Vec3 u{c.up.x*sy,c.up.y*sy,c.up.z*sy};
    const Vec3 d{-c.forward.x/120,-c.forward.y/120,-c.forward.z/120};
    auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    c.matrix={r.x,u.x,d.x,0,r.y,u.y,d.y,0,r.z,u.z,d.z,0,
              -dot(r,c.focus),-dot(u,c.focus),.5f-dot(d,c.focus),1};
    return c;
}

juce::Point<float> IslandScene::project(double u, double v) const {
    return projectLift(u,v,0);
}

juce::Point<float> IslandScene::projectLift(double u,double v,float lift) const {
    const auto c=camera();
    auto p=position(float(u),float(v));p.y+=lift;
    const Vec3 d{p.x-c.focus.x,p.y-c.focus.y,p.z-c.focus.z};
    return {c.field.getCentreX()+(d.x*c.right.x+d.y*c.right.y+d.z*c.right.z)*c.scale,
            c.field.getCentreY()-(d.x*c.up.x+d.y*c.up.y+d.z*c.up.z)*c.scale};
}

juce::Point<double> IslandScene::unproject(juce::Point<float> screen) const {
    const auto c=camera();
    if (!std::isfinite(screen.x)||!std::isfinite(screen.y)) return {NAN,NAN};
    const float dx=(screen.x-c.field.getCentreX())/c.scale;
    const float dy=(screen.y-c.field.getCentreY())/c.scale;
    const Vec3 ray{c.focus.x+c.right.x*dx-c.up.x*dy,c.focus.y+c.right.y*dx-c.up.y*dy,c.focus.z+c.right.z*dx-c.up.z*dy};
    auto point=[&](float t)->Vec3{return {ray.x+c.forward.x*t,ray.y+c.forward.y*t,ray.z+c.forward.z*t};};
    auto separation=[&](float t){const auto p=point(t);return p.y-floorHeight(p);};
    float high=(3*sceneryScale+.25f-ray.y)/c.forward.y;
    float low=high,end=-ray.y/c.forward.y-.01f;
    // Start above the terrain and take short steps so a foreground ridge wins.
    for(int i=0;i<4096;++i){
        const float gap=separation(high);
        if(gap<.00001f){low=high;break;}
        low=std::max(end,high-std::max(.00005f,std::min(.12f,gap/9)));
        if(separation(low)<=0||low<=end)break;
        high=low;
    }
    for(int i=0;i<22;++i){const float mid=(low+high)*.5f;if(separation(mid)>0)high=mid;else low=mid;}
    return coordinates(point((low+high)*.5f));
}

void IslandScene::addTriangle(Vec3 a,Vec3 b,Vec3 c,Vec3 color,float material,Vec3 na,Vec3 nb,Vec3 nc){
    const Vec3 e{b.x-a.x,b.y-a.y,b.z-a.z},f{c.x-a.x,c.y-a.y,c.z-a.z};
    Vec3 n{e.y*f.z-e.z*f.y,e.z*f.x-e.x*f.z,e.x*f.y-e.y*f.x};
    const float inv=1/std::max(.000001f,std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z));n={n.x*inv,n.y*inv,n.z*inv};
    for (auto item : {std::pair<Vec3,Vec3>{a,na},{b,nb},{c,nc}}) {
        const auto p=item.first,q=item.second;
        vertices.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},{color.x,color.y,color.z},{q.x,q.y,q.z},material});
    }
}
void IslandScene::addTriangle(Vec3 a,Vec3 b,Vec3 c,Vec3 color,float material){addTriangle(a,b,c,color,material,a,b,c);}

void IslandScene::addTerrain(){
    // The same square-to-disc embedding and ridge as iOS; the grid is dense
    // enough for continuous silhouette and material, independent of UI size.
    // CoreSimulator's OpenGL ES renderer is software-only; keep its preview
    // responsive enough for gesture testing. Real iPhones retain full density.
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    constexpr int nx=48,nz=30;
#else
    constexpr int nx=160,nz=100;
#endif
    std::vector<Vec3> points,normals;
    points.reserve((nx+1)*(nz+1));normals.reserve((nx+1)*(nz+1));
    for(int j=0;j<=nz;++j)for(int i=0;i<=nx;++i){
        const float u=float(i)/nx,v=.60f*float(j)/nz;
        points.push_back(position(u,v));normals.push_back(normal(u,v));
    }
    auto setNormals=[this](Vec3 a,Vec3 b,Vec3 c){
        const size_t n=vertices.size();
        const Vec3 ns[]={a,b,c};
        for(size_t k=0;k<3;++k){vertices[n-3+k].normal[0]=ns[k].x;
            vertices[n-3+k].normal[1]=ns[k].y;vertices[n-3+k].normal[2]=ns[k].z;}
    };
    for(int j=0;j<nz;++j)for(int i=0;i<nx;++i){
        const int ia=j*(nx+1)+i,ib=ia+1,ic=ia+nx+1,id=ic+1;
        const auto a=points[ia],b=points[ib],c=points[ic],d=points[id];
        const auto na=normals[ia],nb=normals[ib],nc=normals[ic],nd=normals[id];
        const auto color=groundColor(position((float(i)+.5f)/nx,(float(j)+.5f)*.60f/nz));
        addTriangle(a,c,b,color,1);setNormals(na,nc,nb);
        addTriangle(b,c,d,color,1);setNormals(nb,nc,nd);
    }
}

void IslandScene::addSea(){
    // Rest depth rides in color.x so the vertex ABI stays unchanged.
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    constexpr int nx=48,nz=40;
#else
    constexpr int nx=160,nz=130;
#endif
    auto corner=[nx,nz](int i,int j){ return Vec3{-32+float(i)*64/nx,.06f,-26+float(j)*52/nz}; };
    auto tint=[](Vec3 p){ return Vec3{waterDepth(p),0,0}; };
    auto add=[&](Vec3 a,Vec3 b,Vec3 c){
        addTriangle(a,b,c,{0,0,0},3);
        const Vec3 ts[]={tint(a),tint(b),tint(c)};
        const size_t n=vertices.size();
        for(size_t k=0;k<3;++k){auto& vx=vertices[n-3+k];
            vx.color[0]=ts[k].x;vx.normal[0]=0;vx.normal[1]=1;vx.normal[2]=0;}
    };
    for(int j=0;j<nz;++j)for(int i=0;i<nx;++i){
        const auto a=corner(i,j),b=corner(i,j+1),c=corner(i+1,j),d=corner(i+1,j+1);
        add(a,b,c);add(c,b,d);
    }
}

void IslandScene::addRocks(){
    struct Rock {float u,v,x,y,z;int family;};
    const Rock rocks[]={{.24f,.27f,1.06f,1.80f,.74f,0},{.16f,.37f,1.08f,.50f,.67f,4},
        {.34f,.37f,1.21f,.40f,.63f,1},{.77f,.31f,1.06f,1.02f,.80f,1},
        {.85f,.44f,.72f,.39f,.58f,5},{.67f,.39f,.50f,.25f,.50f,2},
        {.48f,.17f,.36f,.87f,.40f,3},{.91f,.19f,.41f,.55f,.53f,0}};
    auto emit=[this](Rock rock,int seed){
        const auto root=position(rock.u,rock.v);
        auto p=[&](int i,int j)->Vec3{
            const float a=2*pi*float(i)/32,t=pi*float(j)/20;
            const float x=std::cos(a)*std::sin(t),y=std::cos(t),z=std::sin(a)*std::sin(t);
            float r=1+.11f*std::sin(3*a+seed)*std::sin(4*t)+.07f*std::cos(7*a-2*t+seed);
            const float rockX=(x-.4f)*3,rockY=(y-.3f)*3;
            if(rock.family==0||rock.family==5)r-=.16f*std::exp(-rockX*rockX-rockY*rockY);
            if(rock.family==1)r+=.07f*std::sin(y*24+seed);
            return {root.x+x*r*rock.x*sceneryScale,root.y+(y*r+.47f)*rock.y*sceneryScale-.11f*sceneryScale,
                    root.z+z*r*rock.z*sceneryScale};
        };
        for(int j=0;j<20;++j)for(int i=0;i<32;++i){
            const auto a=p(i,j),b=p(i+1,j),c=p(i,j+1),d=p(i+1,j+1);
            const Vec3 col{.13f,.145f,.15f};
            addTriangle(a,b,c,col,4);addTriangle(b,d,c,col,4);
        }
    };
    for(int i=0;i<8;++i)emit(rocks[i],i+1);
    uint32_t rng=0x571827da;
    for(int i=0;i<14;++i){
        const float u=.08f+random01(rng)*.84f,v=.43f+random01(rng)*.15f,s=.11f+std::pow(random01(rng),1.7f)*.26f;
        emit({u,v,s*(.7f+random01(rng)*.6f),s*(.3f+random01(rng)*.7f),s*(.55f+random01(rng)*.5f),i%6},i+30);
    }
}

void IslandScene::addLogs(){
    struct Log{const char* data;int size;float u,v,length,width,yaw,roll;};
    const Log logs[]={{BinaryData::FallenLogArch_pfmesh,BinaryData::FallenLogArch_pfmeshSize,.43f,.30f,4.7f,1.08f,.58f,-pi/2},
        {BinaryData::FallenLogFork_pfmesh,BinaryData::FallenLogFork_pfmeshSize,.72f,.19f,3.5f,.96f,-pi+.45f,0},
        {BinaryData::FallenLogBole_pfmesh,BinaryData::FallenLogBole_pfmeshSize,.72f,.50f,2.4f,1.02f,.22f,0}};
    for(const auto& log:logs){
        if(log.size<8||std::memcmp(log.data,"PFM1",4)!=0)continue;
        uint32_t count=0;std::memcpy(&count,log.data+4,4);
        if(count==0||count>200000||count%3||log.size!=8+int(count)*64)continue;
        auto source=[&](uint32_t i,int lane){float value=0;std::memcpy(&value,log.data+8+size_t(i)*64+lane*4,4);return value;};
        float lo=1e9f,hi=-1e9f;
        for(uint32_t i=0;i<count;++i){lo=std::min(lo,source(i,0));hi=std::max(hi,source(i,0));}
        const float length=log.length*sceneryScale,scale=length/std::max(.001f,hi-lo);
        const auto center=position(log.u,log.v);
        const Vec3 dir{std::cos(log.yaw),0,std::sin(log.yaw)};
        const float half=length*.40f;
        const float rise=std::clamp((floorHeight({center.x+dir.x*half,0,center.z+dir.z*half})-
            floorHeight({center.x-dir.x*half,0,center.z-dir.z*half}))/(2*half),-.32f,.32f);
        const float inv=1/std::sqrt(1+rise*rise);
        const Vec3 x{dir.x*inv,rise*inv,dir.z*inv},across{-dir.z,0,dir.x};
        const Vec3 upright{-across.z*x.y,across.z*x.x-across.x*x.z,across.x*x.y};
        const Vec3 y{upright.x*std::cos(log.roll)+across.x*std::sin(log.roll),upright.y*std::cos(log.roll),upright.z*std::cos(log.roll)+across.z*std::sin(log.roll)};
        const Vec3 z{across.x*std::cos(log.roll)-upright.x*std::sin(log.roll),-upright.y*std::sin(log.roll),across.z*std::cos(log.roll)-upright.z*std::sin(log.roll)};
        auto transformed=[&](uint32_t i)->Vec3{
            const float a=source(i,0)*scale,b=source(i,1)*scale*log.width,c=source(i,2)*scale*log.width;
            return {center.x+x.x*a+y.x*b+z.x*c,center.y+x.y*a+y.y*b+z.y*c,center.z+x.z*a+y.z*b+z.z*c};
        };
        float lift=-1e9f;
        for(uint32_t i=0;i<count;i+=3){const auto p=transformed(i);lift=std::max(lift,floorHeight(p)-p.y);}
        lift-=.065f*sceneryScale;
        for(uint32_t i=0;i<count;i+=3){
            Vec3 p[3],rest[3];
            for(int k=0;k<3;++k){p[k]=transformed(i+k);p[k].y+=lift;rest[k]={source(i+k,0)*scale,source(i+k,1)*scale*log.width,source(i+k,2)*scale*log.width};}
            const Vec3 color{source(i,8),source(i,9),source(i,10)};
            addTriangle(p[0],p[1],p[2],color,13,rest[0],rest[1],rest[2]);
        }
    }
}

void IslandScene::addVegetation(){
    uint32_t rng=0x571827da;
    struct Obstacle{float u,v,r;};
    const Obstacle stone[]={{.24f,.27f,1.2f},{.16f,.37f,1.0f},{.34f,.37f,1.05f},
        {.77f,.31f,1.0f},{.85f,.44f,.65f},{.67f,.39f,.48f},{.48f,.17f,.40f},{.91f,.19f,.48f}};
    Vec3 stonePositions[8];
    for(int i=0;i<8;++i)stonePositions[i]=position(stone[i].u,stone[i].v);
    struct WoodFootprint{Vec3 center;float dx,dz,half,radius;};
    const WoodFootprint logs[]={{position(.43f,.30f),std::cos(.58f),std::sin(.58f),4.7f*sceneryScale*.5f,1.08f*sceneryScale*.5f},
        {position(.72f,.19f),std::cos(-pi+.45f),std::sin(-pi+.45f),3.5f*sceneryScale*.5f,.96f*sceneryScale*.5f},
        {position(.72f,.50f),std::cos(.22f),std::sin(.22f),2.4f*sceneryScale*.5f,1.02f*sceneryScale*.5f}};
    auto clearRoot=[&](float u,float v,float margin){
        const auto p=position(u,v);
        for(int i=0;i<8;++i){const auto q=stonePositions[i];
            if(std::hypot(p.x-q.x,p.z-q.z)<stone[i].r*sceneryScale+margin)return false;}
        for(const auto& log:logs){const auto q=log.center;
            const float along=std::clamp((p.x-q.x)*log.dx+(p.z-q.z)*log.dz,-log.half,log.half);
            if(std::hypot(p.x-q.x-log.dx*along,p.z-q.z-log.dz*along)<log.radius+margin)return false;}
        return true;
    };
    // Physical grass ribbons and several leaf families retain real silhouette.
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    constexpr int grassSamples=900;
#else
    constexpr int grassSamples=10000;
#endif
    for(int i=0;i<grassSamples;++i){
        const float u=.045f+random01(rng)*.91f,v=.035f+random01(rng)*.53f;
        const float patch=std::sin(u*25+v*14)*std::cos(u*12-v*29);
        if(patch<-.12f||height(u,v)<=.035f||(u>.40f&&u<.61f&&v>.40f)||!clearRoot(u,v,.045f))continue;
        const auto root=position(u,v);
        const float h=(.09f+random01(rng)*.41f)*(.6f+std::max(0.0f,patch)*.8f)*sceneryScale;
        const float w=(.012f+random01(rng)*.016f)*sceneryScale,a=random01(rng)*2*pi,bend=(random01(rng)-.5f)*h*.80f;
        const Vec3 side{-std::sin(a)*w,0,std::cos(a)*w};
        auto blade=[&](float t,float sign)->Vec3{return {root.x+std::cos(a)*bend*t*t+side.x*sign*(1-t),root.y+h*t,root.z+std::sin(a)*bend*t*t+side.z*sign*(1-t)};};
        const Vec3 color{.18f+random01(rng)*.30f,.46f+random01(rng)*.34f,.025f+random01(rng)*.065f};
        const auto l=blade(0,-1),r=blade(0,1),m=blade(.65f,0),tip=blade(1,0);
        addTriangle(l,r,m,color,5);addTriangle(r,tip,m,color,5);
    }
    auto leaf=[this](Vec3 root,float length,float leafWidth,float angle,float rise,float curl,int lobes,uint32_t seed){
        const float dx=std::cos(angle),dz=std::sin(angle),phase=float(seed%100)*.063f;
        const int nu=length<.38f?5:13,nv=length<.38f?3:5;
        auto point=[&](int i,int j)->Vec3{
            const float t=float(i)/(nu-1),s=float(j)*2/(nv-1)-1;
            const float envelope=std::pow(std::max(0.0f,std::sin(t*pi)),.60f);
            const float lobing=lobes>0?.72f+.28f*std::cos(t*lobes*2*pi+phase):1.0f;
            const float ripple=std::sin(t*19+phase)*std::min(.027f,leafWidth*.13f)*s*s;
            Vec3 p{root.x+dx*length*t-dz*s*leafWidth*envelope*lobing,
                root.y+rise*t+std::sin(t*pi)*length*.20f-curl*t*t*t+
                    (std::abs(s)*leafWidth*(.16f+.18f*std::sin(t*7+phase))+ripple)*envelope,
                root.z+dz*length*t+dx*s*leafWidth*envelope*lobing};
            p.y=std::max(p.y,floorHeight(p)+.065f*sceneryScale);
            return p;
        };
        for(int i=0;i<nu-1;++i)for(int j=0;j<nv-1;++j){
            const float t=float(i)/(nu-1);
            const Vec3 color{.10f+t*.26f,.62f+t*.26f,.045f+float(seed%5)*.014f};
            const auto a=point(i,j),b=point(i+1,j),c=point(i,j+1),d=point(i+1,j+1);
            addTriangle(a,b,c,color,6);addTriangle(b,d,c,color,6);
        }
    };
    const float sites[][3]={{.12f,.31f,1.30f},{.31f,.13f,.55f},{.40f,.32f,.80f},{.66f,.15f,1.1f},
        {.87f,.43f,.62f},{.89f,.23f,1.25f},{.48f,.07f,.50f},{.72f,.48f,.85f},
        {.14f,.23f,1.35f},{.36f,.10f,.62f},{.72f,.22f,1.05f},{.83f,.36f,.86f},
        {.10f,.46f,.56f},{.65f,.50f,.68f},{.43f,.24f,.46f},{.78f,.10f,.90f},{.29f,.44f,.55f}};
    for(int s=0;s<17;++s){
        float u=sites[s][0],v=sites[s][1];
        if(!clearRoot(u,v,.10f)){
            bool placed=false;
            for(int ring=1;ring<=8&&!placed;++ring)for(int j=0;j<16&&!placed;++j){
                const float a=2*pi*float(j)/16;
                const float candidateU=std::clamp(u+std::cos(a)*ring*.025f,.04f,.96f);
                const float candidateV=std::clamp(v+std::sin(a)*ring*.018f,.03f,.57f);
                if(height(candidateU,candidateV)>.035f&&clearRoot(candidateU,candidateV,.10f)){
                    u=candidateU;v=candidateV;placed=true;
                }
            }
            if(!placed)continue;
        }
        const auto root=position(u,v);
        const float size=sites[s][2]*sceneryScale;
        if(s<8){
            const int fronds=4+int(random01(rng)*5);
            for(int j=0;j<fronds;++j){
                const float a=float(j)*2.399f+random01(rng),dx=std::cos(a),dz=std::sin(a);
                const float reach=size*(.55f+random01(rng)*.65f),rise=size*(.40f+random01(rng)*.5f);
                auto curve=[&](float t)->Vec3{
                    const float q=1-t;
                    const Vec3 b{root.x,root.y+rise,root.z};
                    const Vec3 c{root.x+dx*reach*.56f,root.y+rise,root.z+dz*reach*.56f};
                    const Vec3 d{root.x+dx*reach,root.y+rise*.48f,root.z+dz*reach};
                    return {root.x*q*q*q+3*b.x*q*q*t+3*c.x*q*t*t+d.x*t*t*t,
                        root.y*q*q*q+3*b.y*q*q*t+3*c.y*q*t*t+d.y*t*t*t,
                        root.z*q*q*q+3*b.z*q*q*t+3*c.z*q*t*t+d.z*t*t*t};
                };
                for(int k=2;k<=12;++k){
                    const float t=float(k)/14,len=reach*std::sin(t*pi)*(.20f+random01(rng)*.07f);
                    const auto center=curve(t);
                    for(float sign:{-1.0f,1.0f}){
                        const float ax=-dz*sign+dx*.3f,az=dx*sign+dz*.3f;
                        leaf(center,len,len*.16f,std::atan2(az,ax),len*.15f,len*.15f,0,rng+uint32_t(k));
                    }
                }
            }
        } else {
            const int family=(s-8)%3;
            const int count=family==0?5+int(random01(rng)*4):family==1?9+int(random01(rng)*5):4+int(random01(rng)*4);
            const float angle0=random01(rng)*2*pi;
            for(int j=0;j<count;++j){
                const float a=angle0+j*2.39996f+(random01(rng)-.5f)*.35f;
                const float len=size*(.55f+random01(rng)*.9f);
                const float w=len*(family==0?.25f:family==1?.055f:.15f)*(.75f+random01(rng)*.6f);
                const float rise=len*(family==1?.9f:.55f+random01(rng)*.35f);
                const int lobes=family==0?3+int(random01(rng)*3):0;
                leaf({root.x,root.y+j*.012f,root.z},len,w,a,rise,len*(family==2?.35f:.20f),lobes,rng);
            }
        }
    }
    auto thinStem=[this](Vec3 a,Vec3 b,float w,Vec3 color){
        addTriangle({a.x-w,a.y,a.z},{b.x-w,b.y,b.z},{a.x+w,a.y,a.z},color,5);
        addTriangle({a.x+w,a.y,a.z},{b.x-w,b.y,b.z},{b.x+w,b.y,b.z},color,5);
        addTriangle({a.x,a.y,a.z-w},{b.x,b.y,b.z-w},{a.x,a.y,a.z+w},color,5);
        addTriangle({a.x,a.y,a.z+w},{b.x,b.y,b.z-w},{b.x,b.y,b.z+w},color,5);
    };
    auto fruit=[this](Vec3 p,float r,Vec3 color){
        const Vec3 top{p.x,p.y+r,p.z},bottom{p.x,p.y-r,p.z};
        const Vec3 ring[]={{p.x+r,p.y,p.z},{p.x,p.y,p.z+r},{p.x-r,p.y,p.z},{p.x,p.y,p.z-r}};
        for(int j=0;j<4;++j){addTriangle(top,ring[j],ring[(j+1)%4],color,5);
            addTriangle(bottom,ring[(j+1)%4],ring[j],color,5);}
    };
    // The iOS seed heads add a fine ochre layer between the tall leaves and
    // ground mesh; their points are geometry, not extra ecology entities.
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    constexpr int fleckSamples=60;
#else
    constexpr int fleckSamples=330;
#endif
    for(int i=0;i<fleckSamples;++i){
        const float u=.08f+random01(rng)*.84f,v=.10f+random01(rng)*.44f;
        if(std::sin(u*18-v*12)<.10f||height(u,v)<=.035f||!clearRoot(u,v,.035f))continue;
        const auto root=position(u,v);
        const float h=(.24f+std::pow(random01(rng),1.4f)*.9f)*sceneryScale;
        const Vec3 joint{root.x+(random01(rng)-.5f)*.12f*sceneryScale,
                         root.y+h*.70f,root.z};
        thinStem(root,joint,.006f*sceneryScale,{.40f,.57f,.16f});
        const int count=2+int(random01(rng)*5);
        for(int j=0;j<count;++j){
            const float a=float(j)*2.399f+random01(rng)*.6f;
            const Vec3 tip{joint.x+std::cos(a)*(.06f+random01(rng)*.12f)*sceneryScale,
                           joint.y+h*(.18f+random01(rng)*.35f),
                           joint.z+std::sin(a)*.12f*sceneryScale};
            thinStem(joint,tip,.003f*sceneryScale,{.57f,.66f,.35f});
            const float size=(.025f+random01(rng)*.020f)*sceneryScale;
            fruit(tip,size,{.62f+random01(rng)*.15f,.45f+random01(rng)*.20f,.12f});
        }
    }
}

void IslandScene::buildGeometry(){
    if(geometryReady)return;
    vertices.reserve(500000);
    addTerrain();addRocks();addLogs();addVegetation();
    seaFirst=vertices.size();
    addSea();
    geometryReady=true;
}

void IslandScene::newOpenGLContextCreated(){
    buildGeometry();
    static const char* vertexSource=R"glsl(
        attribute vec3 aPosition; attribute vec3 aNormal; attribute vec3 aColor;
        attribute vec3 aNetPosition; attribute float aMaterial;
        uniform mat4 uMatrix; varying vec3 vWorld; varying vec3 vNormal;
        varying vec3 vColor; varying vec3 vNet; varying float vMaterial;
        uniform vec4 uCrestA[8]; uniform vec4 uCrestB[8];
        float crestHeight(vec2 p){
            float h=0.0;
            for(int i=0;i<8;++i){
                vec2 d=uCrestA[i].zw,side=vec2(-d.y,d.x),delta=(p-uCrestA[i].xy)/1.5;
                float across=dot(delta,side);
                float x=(dot(delta,d)-.14*across*across)/uCrestB[i].y,z=across/uCrestB[i].x;
                float support=max(0.0,1.0-x*x-z*z);
                h+=uCrestB[i].z*support*support*support;
            }
            return h;
        }
        void main(){ vec3 p=aPosition;
            if(aMaterial>2.5&&aMaterial<3.5)p.y+=crestHeight(p.xz);
            vWorld=p;vNormal=aNormal;vColor=aColor;
            vNet=aNetPosition;vMaterial=aMaterial;gl_Position=uMatrix*vec4(p,1.0); }
    )glsl";
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    static const char* fragmentSource=R"glsl(
        precision mediump float;
        varying vec3 vNormal;varying vec3 vColor;varying float vMaterial;
        uniform float uDay;
        void main(){
            float light=.50+.30*max(0.0,dot(normalize(vNormal),normalize(vec3(-.42,.85,.33))))+.15*uDay;
            if(vMaterial>2.5&&vMaterial<3.5){
                float waterDepth=max(0.0,vColor.x),shore=smoothstep(0.0,.22,waterDepth);
                if(shore<=.0001)discard;
                float deep=1.0-exp(-waterDepth*.65);
                vec3 water=mix(vec3(.035,.155,.120),vec3(.012,.068,.074),deep)*(.42+.58*uDay);
                gl_FragColor=vec4(pow(water,vec3(1.0/2.2)),shore*(.26+deep*.27));return;
            }
            vec3 color=vColor*light;
            if(vMaterial<1.5)color*=.45;
            gl_FragColor=vec4(color,1.0);
        }
    )glsl";
#else
    static const char* fragmentSource=R"glsl(
        #ifdef GL_ES
        #extension GL_OES_standard_derivatives : enable
        precision highp float;
        #endif
        varying vec3 vWorld;varying vec3 vNormal;varying vec3 vColor;
        varying vec3 vNet;varying float vMaterial;
        uniform float uDay;uniform float uTime;uniform float uWind;uniform vec3 uView;
        uniform vec4 uCrestA[8];uniform vec4 uCrestB[8];
        // WorldShaders.metal reliefSurface (calm): height and dHeight/dx,dz.
        vec3 crestSurface(vec2 p){
            float h=0.0;vec2 slope=vec2(0.0);
            for(int i=0;i<8;++i){
                vec2 d=uCrestA[i].zw,side=vec2(-d.y,d.x),delta=(p-uCrestA[i].xy)/1.5;
                float len=uCrestB[i].x,breadth=uCrestB[i].y,amplitude=uCrestB[i].z;
                float along=dot(delta,d),across=dot(delta,side);
                float x=(along-.14*across*across)/breadth,z=across/len,radial=x*x+z*z;
                if(radial<1.0){
                    float support=1.0-radial;h+=amplitude*support*support*support;
                    vec2 gradient=d*(2.0*x/breadth)+side*(2.0*z/len-.56*x*across/breadth);
                    slope+=gradient*(-3.0*amplitude*support*support)/1.5;
                }
            }
            return vec3(h,slope);
        }
        // WorldShaders.metal materialWaterFragment, without the rain tint and timeGrade
        // that the rest of this scene does not apply either.
        vec4 waterColor(vec3 world,float restDepth){
            vec2 p=world.xz;float time=uTime,windAmount=clamp(uWind,0.0,1.0),day=clamp(uDay,0.0,1.0);
            vec2 bendA=vec2(.16,-.08),bendB=vec2(.06,.19);
            float a=dot(p,bendA)-time*.035,b=dot(p,bendB)+time*.024+1.7;
            float warp=.90*sin(a)+.40*sin(b);
            vec2 warpSlope=.90*cos(a)*bendA+.40*cos(b)*bendB;
            float speed=.55+windAmount*.38;
            vec3 wavePhase=vec3(dot(p,vec2(1.28,.54))-time*speed+warp,
                                dot(p,vec2(1.81,1.22))-time*speed*1.31+warp*.73+2.1,
                                dot(p,vec2(3.51,.62))-time*speed*1.67+warp*1.12+4.4);
            vec3 g=cos(wavePhase);
            vec2 slope=g.x*(vec2(1.28,.54)+warpSlope)
                      +g.y*(vec2(1.81,1.22)+warpSlope*.73)*.28
                      +g.z*(vec2(3.51,.62)+warpSlope*1.12)*.075;
            vec3 surge=crestSurface(p);
            slope=slope*(.022+windAmount*.052)+surge.yz;
            vec3 n=normalize(vec3(-slope.x,1.0,-slope.y));
            float facing=clamp(abs(dot(n,normalize(uView))),0.0,1.0);
            float fresnel=.035+.42*pow(1.0-facing,4.0);
            float waterDepth=restDepth+surge.x;
            float shore=smoothstep(0.0,.22,waterDepth);
            float deep=1.0-exp(-waterDepth*.65);
            vec3 pigment=mix(vec3(.035,.155,.120),vec3(.012,.068,.074),deep);
            float lift=max(0.0,surge.x),relief=clamp(lift/1.15,0.0,1.0);
            float facingLight=.52+.48*max(0.0,dot(n,normalize(vec3(-.45,.82,.34))));
            float contours=.5+.5*cos(lift*25.0+dot(p,vec2(.10,.06)));
            float engraving=mix(1.0,.72+.28*smoothstep(.2,.8,contours),smoothstep(.08,.42,relief));
            pigment=mix(pigment,vec3(.075,.34,.29)*facingLight,relief*.83)*engraving;
            vec3 sky=mix(vec3(.022,.043,.063),vec3(.16,.26,.25),day);
            vec3 color=mix(pigment*(.42+.58*day),sky,fresnel);
            float alpha=shore*clamp(.26+deep*.27+fresnel*.20+relief*.20,0.0,.78);
            // iOS writes linear values to an sRGB drawable; this framebuffer is not sRGB.
            return vec4(pow(color,vec3(1.0/2.2)),alpha);
        }
        vec3 latticeLines(vec3 phase,vec3 footprint,float scale){
            vec3 q=phase*scale,w=max(footprint*scale,vec3(.00001));
            vec3 distance=abs(fract(q+.5)-.5);
            vec3 halfWidth=min(vec3(.075),w*.32);
            return 1.0-smoothstep(max(vec3(0.0),halfWidth-w*.5),halfWidth+w*.5,distance);
        }
        void main(){
            if(vMaterial>2.5&&vMaterial<3.5){
                if(vColor.x<=.0001)discard;
                gl_FragColor=waterColor(vWorld,vColor.x);
                if(gl_FragColor.a<=.0001)discard;
                return;
            }
            vec3 n=normalize(vNormal);float diffuse=max(0.0,dot(n,normalize(vec3(-.42,.85,.33))));
            float light=.46+diffuse*.42+uDay*.20;
            if(vMaterial>4.5&&vMaterial<6.5){
                gl_FragColor=vec4(vColor*light*.95,1.0);return;
            }
            vec3 frequency=vMaterial>12.0?vec3(14.0,24.0,24.0):vec3(18.0);
            vec3 phase=frequency*(vNet+.36*sin(vNet*.5+vec3(.2,.7,1.1)))+vec3(.31,.47,.63);
            vec3 width=fwidth(phase);
            vec3 geometricNormal=cross(dFdx(vNet),dFdy(vNet));
            geometricNormal=length(geometricNormal)>.0000001?abs(normalize(geometricNormal)):vec3(0.0);
            vec3 diagonalPhase=phase.yzx-phase.zxy;
            vec3 diagonalWidth=fwidth(diagonalPhase);
            vec3 plane=geometricNormal*geometricNormal;plane*=plane;plane*=plane;
            plane/=max(plane.x+plane.y+plane.z,.000001);
            vec3 effectiveWidth=max(width,diagonalWidth*plane);
            float footprint=max(effectiveWidth.x,max(effectiveWidth.y,effectiveWidth.z));
            float lod=max(0.0,log2(max(footprint,.00001)/.30));
            float scale=exp2(-floor(lod));
            float blend=smoothstep(0.0,1.0,fract(lod));
            vec3 lines=mix(latticeLines(phase,width,scale),latticeLines(phase,width,scale*.5),blend);
            vec3 diag=mix(latticeLines(diagonalPhase,diagonalWidth,scale),
                          latticeLines(diagonalPhase,diagonalWidth,scale*.5),blend)*plane;
            lines*=1.0-smoothstep(vec3(.55),vec3(.98),geometricNormal);
            float axes=1.0-(1.0-lines.x)*(1.0-lines.y)*(1.0-lines.z);
            float diagonal=diag.x+diag.y+diag.z;
            float coverage=1.0-(1.0-axes)*(1.0-diagonal);
            float variation=.82+.18*sin(vWorld.x*.82+vWorld.z*.51+vWorld.y*1.2);
            vec3 filament=vColor*variation*light*1.7;
            if(vMaterial<1.5)filament=mix(vColor,vec3(.05,.08,.07),.55)*variation*light*2.2;
            if(vMaterial>3.5&&vMaterial<4.5)filament=min(filament,vec3(.23,.25,.27));
            if(vMaterial>12.0)filament*=vec3(.70,1.0,1.20);
            vec3 backing=vec3(.0022,.0027,.0024);
            gl_FragColor=vec4(mix(backing,filament,coverage),1.0);
        }
    )glsl";
#endif
    shader=std::make_unique<juce::OpenGLShaderProgram>(context);
    if(!shader->addVertexShader(vertexSource)||!shader->addFragmentShader(fragmentSource)||!shader->link()){
        juce::Logger::writeToLog("Island shader: "+shader->getLastError());shader.reset();return;
    }
    context.extensions.glGenBuffers(1,&vertexBuffer);
    context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,vertexBuffer);
    context.extensions.glBufferData(juce::gl::GL_ARRAY_BUFFER,GLsizeiptr(vertices.size()*sizeof(Vertex)),vertices.data(),juce::gl::GL_STATIC_DRAW);

    // The same three topology families and twelve variants as the Metal app.
    std::vector<OrganismMesh::Vertex> organismVertices;
    for(int kind=0;kind<3;++kind)for(int variant=0;variant<OrganismMesh::variantCount;++variant){
        auto mesh=OrganismMesh::build(kind,variant);
        auto& span=organismSpans[size_t(kind*OrganismMesh::variantCount+variant)];
        span.first=int(organismVertices.size());span.count=int(mesh.size());
        organismVertices.insert(organismVertices.end(),mesh.begin(),mesh.end());
    }
    static const char* organismVertex=R"glsl(
        attribute vec3 aPosition; attribute vec3 aNormal; attribute vec3 aColor; attribute float aEmission;
        uniform mat4 uMatrix; uniform mat4 uModel; uniform vec4 uTint;
        varying vec3 vNormal; varying vec3 vColor; varying float vEmission;
        void main(){vec4 world=uModel*vec4(aPosition,1.0);gl_Position=uMatrix*world;
            // w=0 excludes translation and works in desktop GLSL 1.10 and GLES 2.
            vNormal=normalize((uModel*vec4(aNormal,0.0)).xyz);vColor=aColor*uTint.rgb;vEmission=aEmission;}
    )glsl";
    static const char* organismFragment=R"glsl(
        #ifdef GL_ES
        precision highp float;
        #endif
        uniform float uDay;uniform float uEnergy;uniform float uHighlight;
        uniform vec3 uView;uniform vec4 uTint;
        varying vec3 vNormal;varying vec3 vColor;varying float vEmission;
        void main(){float diffuse=max(0.0,dot(normalize(vNormal),normalize(vec3(-.45,.85,-.35))));
            float lit=.24+uDay*.34+diffuse*.42;
            float strength=mix(lit,1.0,max(.80,max(vEmission,uEnergy)));
            float edge=pow(1.0-abs(dot(normalize(vNormal),normalize(uView))),2.0);
            vec3 color=mix(vColor*strength,vec3(.82,1.0,.52),edge*uHighlight*.85);
            gl_FragColor=vec4(color,uTint.a);}
    )glsl";
    organismShader=std::make_unique<juce::OpenGLShaderProgram>(context);
    if(!organismShader->addVertexShader(organismVertex)||!organismShader->addFragmentShader(organismFragment)||!organismShader->link()){
        juce::Logger::writeToLog("Organism shader: "+organismShader->getLastError());organismShader.reset();
    }else{
        context.extensions.glGenBuffers(1,&organismBuffer);
        context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,organismBuffer);
        context.extensions.glBufferData(juce::gl::GL_ARRAY_BUFFER,GLsizeiptr(organismVertices.size()*sizeof(OrganismMesh::Vertex)),organismVertices.data(),juce::gl::GL_STATIC_DRAW);
    }
    context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,0);
}

void IslandScene::drawOrganisms(const Camera& c){
    if(!organismShader||!organismBuffer)return;
    std::vector<Organism> snapshot;
    {std::lock_guard<std::mutex> lock(organismMutex);snapshot=organisms;}
    organismShader->use();
    organismShader->setUniformMat4("uMatrix",c.matrix.data(),1,juce::gl::GL_FALSE);
    organismShader->setUniform("uDay",daylight.load());
    organismShader->setUniform("uView",c.forward.x,c.forward.y,c.forward.z);
    context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,organismBuffer);
    const char* names[]={"aPosition","aNormal","aColor","aEmission"};
    const int sizes[]={3,3,3,1};
    const size_t offsets[]={offsetof(OrganismMesh::Vertex,position),offsetof(OrganismMesh::Vertex,normal),offsetof(OrganismMesh::Vertex,color),offsetof(OrganismMesh::Vertex,emission)};
    for(int i=0;i<4;++i){const auto loc=context.extensions.glGetAttribLocation(organismShader->getProgramID(),names[i]);
        if(loc>=0){context.extensions.glEnableVertexAttribArray(GLuint(loc));
            context.extensions.glVertexAttribPointer(GLuint(loc),sizes[i],juce::gl::GL_FLOAT,juce::gl::GL_FALSE,sizeof(OrganismMesh::Vertex),reinterpret_cast<const void*>(offsets[i]));}}
    juce::gl::glEnable(juce::gl::GL_BLEND);
    juce::gl::glBlendFunc(juce::gl::GL_SRC_ALPHA,juce::gl::GL_ONE_MINUS_SRC_ALPHA);
    for(const auto& o:snapshot){
        if(o.kind<0||o.kind>2||o.opacity<=.001f)continue;
        const auto a=OrganismMesh::appearance(o.seed);
        int variant=a.variant;
        if(o.kind==0)variant=std::clamp(o.species,0,2)+3*(a.variant/3);
        const auto span=organismSpans[size_t(o.kind*OrganismMesh::variantCount+variant)];
        if(!span.count)continue;
        Vec3 p=position(float(o.u),float(o.v));
        if(o.kind==0)p.y=std::max(.14f,p.y+.12f); // No ported fish resting depth yet; keep fish at the surface.
        if(o.kind==2)p.y+=o.altitude>0?o.altitude:.95f;
        if(o.hasMotion)p.y=o.motionHeight;
        float angle=a.angle;
        if(o.kind==0){
            const auto end=position(float(o.u)+std::cos(o.heading)*.001f,float(o.v)+std::sin(o.heading)*.001f);
            angle=-std::atan2(end.z-p.z,end.x-p.x);
        }else if(o.kind==2)angle+=o.heading;
        const float size=(o.kind==1?1.70f:1.42f)*(o.kind==0?o.size:1.f)*(1+std::min(.22f,std::max(0.f,o.energy)*.4f));
        const auto scale=a.proportions*size*o.motionGrowth;
        const float cs=std::cos(angle),sn=std::sin(angle),lean=o.bend*(o.kind==1?1.f:.15f);
        std::array<float,16> model{cs*scale.x,0,-sn*scale.x,0,
            lean*.28f*scale.y,scale.y,-lean*.96f*scale.y,0,
            sn*scale.z,0,cs*scale.z,0,p.x,p.y,p.z,1};
        if(o.hasMotion){
            const float x=o.qx,y=o.qy,z=o.qz,w=o.qw;
            const float rotation[9]={1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),
                2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),
                2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)};
            for(int i=0;i<3;++i){model[size_t(i*4)]=rotation[i*3]*(&scale.x)[i];model[size_t(i*4+1)]=rotation[i*3+1]*(&scale.x)[i];model[size_t(i*4+2)]=rotation[i*3+2]*(&scale.x)[i];}
        }
        organismShader->setUniformMat4("uModel",model.data(),1,juce::gl::GL_FALSE);
        organismShader->setUniform("uTint",a.tint.x,a.tint.y,a.tint.z,o.opacity);
        organismShader->setUniform("uEnergy",std::clamp(o.energy,0.f,1.f));
        organismShader->setUniform("uHighlight",o.highlight);
        juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES,span.first,span.count);
    }
    juce::gl::glDisable(juce::gl::GL_BLEND);
    for(const char* name:names){const auto loc=context.extensions.glGetAttribLocation(organismShader->getProgramID(),name);
        if(loc>=0)context.extensions.glDisableVertexAttribArray(GLuint(loc));}
    context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,0);
}

void IslandScene::renderOpenGL(){
    juce::gl::glClearColor(.005f,.015f,.018f,1);
    juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT|juce::gl::GL_DEPTH_BUFFER_BIT);
    if(!shader||!vertexBuffer)return;
    const auto c=camera();
    const auto factor=context.getRenderingScale();
    juce::gl::glViewport(int(c.field.getX()*factor),int((viewHeight.load()-c.field.getBottom())*factor),
                         int(c.field.getWidth()*factor),int(c.field.getHeight()*factor));
    juce::gl::glEnable(juce::gl::GL_DEPTH_TEST);
    juce::gl::glDepthFunc(juce::gl::GL_LEQUAL);
    shader->use();
    shader->setUniformMat4("uMatrix",c.matrix.data(),1,juce::gl::GL_FALSE);
    shader->setUniform("uDay",daylight.load());
    uploadWaterUniforms(c);
    bindTerrainAttributes(true);
    juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES,0,GLsizei(seaFirst));
    bindTerrainAttributes(false);
    drawOrganisms(c);
    // iOS draws the alpha-blended water after the bed and swimming life.
    shader->use();
    bindTerrainAttributes(true);
    juce::gl::glEnable(juce::gl::GL_BLEND);
    juce::gl::glBlendFunc(juce::gl::GL_SRC_ALPHA,juce::gl::GL_ONE_MINUS_SRC_ALPHA);
    juce::gl::glDepthMask(juce::gl::GL_FALSE);
    juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES,GLint(seaFirst),GLsizei(vertices.size()-seaFirst));
    juce::gl::glDepthMask(juce::gl::GL_TRUE);
    bindTerrainAttributes(false);
    juce::gl::glDisable(juce::gl::GL_DEPTH_TEST);
}

void IslandScene::bindTerrainAttributes(bool enable){
    const char* names[]={"aPosition","aNormal","aColor","aNetPosition","aMaterial"};
    const int sizes[]={3,3,3,3,1};
    const size_t offsets[]={offsetof(Vertex,position),offsetof(Vertex,normal),offsetof(Vertex,color),offsetof(Vertex,netPosition),offsetof(Vertex,material)};
    context.extensions.glBindBuffer(juce::gl::GL_ARRAY_BUFFER,enable?vertexBuffer:0);
    for(int i=0;i<5;++i){
        const auto loc=context.extensions.glGetAttribLocation(shader->getProgramID(),names[i]);
        if(loc<0)continue;
        if(!enable){context.extensions.glDisableVertexAttribArray(GLuint(loc));continue;}
        context.extensions.glEnableVertexAttribArray(GLuint(loc));
        context.extensions.glVertexAttribPointer(GLuint(loc),sizes[i],juce::gl::GL_FLOAT,juce::gl::GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsets[i]));
    }
}

// The eight calm crests of WorldShaders.metal reliefSurface, evaluated per frame.
void IslandScene::uploadWaterUniforms(const Camera& c){
    const float time=waterTime.load();
    GLfloat crestA[32],crestB[32];
    for(int i=0;i<8;++i){
        const auto wave=WaterCrests::patch(i,time);
        crestA[i*4+0]=wave.x;
        crestA[i*4+1]=wave.z;
        crestA[i*4+2]=wave.dx;crestA[i*4+3]=wave.dz;
        crestB[i*4+0]=wave.length;
        crestB[i*4+1]=wave.width;
        crestB[i*4+2]=wave.height;
        crestB[i*4+3]=0;
    }
    const auto program=shader->getProgramID();
    juce::gl::glUniform4fv(juce::gl::glGetUniformLocation(program,"uCrestA"),8,crestA);
    juce::gl::glUniform4fv(juce::gl::glGetUniformLocation(program,"uCrestB"),8,crestB);
    shader->setUniform("uTime",time);
    shader->setUniform("uWind",wind.load());
    shader->setUniform("uView",c.forward.x,c.forward.y,c.forward.z);
}

void IslandScene::openGLContextClosing(){
    if(vertexBuffer){context.extensions.glDeleteBuffers(1,&vertexBuffer);vertexBuffer=0;}
    if(organismBuffer){context.extensions.glDeleteBuffers(1,&organismBuffer);organismBuffer=0;}
    shader.reset();
    organismShader.reset();
}
