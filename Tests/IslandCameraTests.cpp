#include "../Source/IslandScene.h"
#include "../Source/OrganismMesh.h"
#include <cmath>
#include <iostream>

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    IslandScene scene;
    scene.resized(852, 393);
    // Visible dry points must survive the same camera projection used for
    // organism circles and the height-field ray used for dragging.
    for (const auto point : {juce::Point<double>(.50,.43),
                             juce::Point<double>(.28,.38),
                             juce::Point<double>(.72,.40)}) {
        const auto screen=scene.project(point.x,point.y);
        const auto result=scene.unproject(screen);
        if (!std::isfinite(result.x)||!std::isfinite(result.y)||
            point.getDistanceFrom(result)>.015) {
            std::cerr << "Camera round-trip failed: " << point.x << "," << point.y
                      << " -> " << result.x << "," << result.y << "\n";
            return 1;
        }
    }
    // Orbit and zoom share that same inverse; neither changes saved UVs.
    scene.orbit({300,170},{345,185});
    scene.navigate({230,170},{630,170},{225,165},{635,175});
    scene.zoomAt({345,160},1.25f);
    const juce::Point<double> point(.50,.43);
    const auto result=scene.unproject(scene.project(point.x,point.y));
    if (point.getDistanceFrom(result)>.015) return 2;
    // Every iOS organism family/variant produces physical triangles, with a
    // stable seed mapping independent of the mutable ecology random stream.
    for(int kind=0;kind<3;++kind)for(int variant=0;variant<OrganismMesh::variantCount;++variant){
        const auto mesh=OrganismMesh::build(kind,variant);
        if(mesh.empty()||mesh.size()%3!=0){std::cerr<<"Missing organism mesh\n";return 3;}
    }
    const auto a=OrganismMesh::appearance(0x12345678u),b=OrganismMesh::appearance(0x12345678u);
    if(a.variant!=b.variant||a.proportions.x!=b.proportions.x||a.tint.z!=b.tint.z||a.angle!=b.angle)return 4;
    return 0;
}
