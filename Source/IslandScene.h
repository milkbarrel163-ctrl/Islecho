#pragma once

#include <juce_opengl/juce_opengl.h>
#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include "OrganismMesh.h"

// The saved (u,v) coordinates are the same as LakeWorldSurface.swift.  This
// class owns presentation only; ecology and audio never depend on the camera.
class IslandScene final : private juce::OpenGLRenderer {
public:
    struct Organism {
        int kind=0;
        double u=.5,v=.5;
        uint32_t seed=1;
        int species=0;
        float size=1,heading=0,energy=0,opacity=1,bend=0,altitude=0;
        float highlight=0;
        bool hasMotion=false;
        float motionHeight=0,motionGrowth=1,qx=0,qy=0,qz=0,qw=1;
    };
    IslandScene();
    ~IslandScene() override;
    void attach(juce::Component&);
    void detach();
    void resized(int width, int height);
    juce::Point<float> project(double u, double v) const;
    juce::Point<float> projectLift(double u,double v,float lift) const;
    juce::Point<double> unproject(juce::Point<float> screen) const;
    void orbit(juce::Point<float> from, juce::Point<float> to);
    void navigate(juce::Point<float> oldA, juce::Point<float> oldB,
                  juce::Point<float> newA, juce::Point<float> newB);
    void setDaylight(float value);
    void setWater(float elapsedSeconds, float windAmount);
    void setOrganisms(std::vector<Organism> value);
    void zoomAt(juce::Point<float> anchor,float factor);
    void repaint();

    // Terrain height at UV coordinates (u,v); returns 0 for water/out-of-bounds.
    // Pure function — safe to call from any thread.
    static float heightAt(float u, float v);

private:
    struct Vec3 { float x, y, z; };
    struct Vertex {
        float position[3], normal[3], color[3], netPosition[3], material;
    };
    struct Camera {
        Vec3 right, up, forward, focus;
        juce::Rectangle<float> field;
        float scale = 1;
        std::array<float, 16> matrix{};
    };

    static constexpr float sceneryScale = 1.5f;
    static constexpr float width = 24.0f;
    static constexpr float depth = 18.0f;
    static float height(float u, float v);
    static float bend(float u);
    static Vec3 position(float u, float v);
    static juce::Point<double> coordinates(Vec3 p);
    static Vec3 normal(float u, float v);
    static Vec3 groundColor(Vec3 p);
    static float floorHeight(Vec3 p);
    static float waterDepth(Vec3 p);
    Camera camera() const;
    Vec3 focusPlanePoint(juce::Point<float> screen) const;

    void addTriangle(Vec3 a, Vec3 b, Vec3 c, Vec3 color, float material,
                     Vec3 netA, Vec3 netB, Vec3 netC);
    void addTriangle(Vec3 a, Vec3 b, Vec3 c, Vec3 color, float material);
    void buildGeometry();
    void addTerrain();
    void addRocks();
    void addLogs();
    void addVegetation();
    void addSea();
    void bindTerrainAttributes(bool enable);
    void uploadWaterUniforms(const Camera&);
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void drawOrganisms(const Camera&);

    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram> organismShader;
    std::vector<Vertex> vertices;
    GLuint vertexBuffer = 0;
    GLuint organismBuffer = 0;
    struct OrganismSpan { int first=0,count=0; };
    std::array<OrganismSpan,3*OrganismMesh::variantCount> organismSpans{};
    std::mutex organismMutex;
    std::vector<Organism> organisms;
    std::atomic<int> viewWidth{1200}, viewHeight{800};
    std::atomic<float> yaw{.12f}, elevation{.93f}, zoom{1.0f}, centerX{0}, centerZ{0}, daylight{.65f};
    std::atomic<float> waterTime{0}, wind{0};
    // Opaque scenery is [0, seaFirst); the translucent sea is drawn last.
    size_t seaFirst = 0;
    bool geometryReady = false;
};
