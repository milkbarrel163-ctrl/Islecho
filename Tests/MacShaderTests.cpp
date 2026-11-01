// Compile the actual app shader strings with the default macOS legacy context.
// A successful C++ build does not compile GLSL; this catches runtime assertions.
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const std::regex pattern(R"pattern(static const char\* (\w+)=R"glsl\(([\s\S]*?)\)glsl";)pattern");
    std::map<std::string, std::string> shaders;
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
        // The full desktop/device terrain fragment follows the simulator branch.
        shaders[(*it)[1].str()] = (*it)[2].str();
    for (const auto* name : {"vertexSource", "fragmentSource", "organismVertex", "organismFragment"})
        if (shaders[name].empty()) {
            std::cerr << "Missing app shader: " << name << '\n';
            return 2;
        }

    const CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAllowOfflineRenderers, static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format = nullptr;
    CGLContextObj context = nullptr;
    GLint count = 0;
    auto error = CGLChoosePixelFormat(attributes, &format, &count);
    if (error == kCGLNoError) error = CGLCreateContext(format, nullptr, &context);
    if (format != nullptr) CGLDestroyPixelFormat(format);
    if (error != kCGLNoError || context == nullptr) {
        std::cerr << "Cannot create shader test context: " << CGLErrorString(error) << '\n';
        return 2;
    }
    CGLSetCurrentContext(context);
    std::cout << "OpenGL " << glGetString(GL_VERSION)
              << "; GLSL " << glGetString(GL_SHADING_LANGUAGE_VERSION) << '\n';
    int failures = 0;
    const auto checkProgram = [&](const char* vertexName, const char* fragmentName) {
        const GLuint program = glCreateProgram();
        for (const auto* name : {vertexName, fragmentName}) {
            const GLuint shader = glCreateShader(name == vertexName ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
            const char* code = shaders.at(name).c_str();
            glShaderSource(shader, 1, &code, nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            char log[16384] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cout << name << ": " << (ok ? "PASS" : "FAIL") << '\n' << log;
            failures += !ok;
            glAttachShader(program, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        char log[16384] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::cout << vertexName << " + " << fragmentName << " link: "
                  << (ok ? "PASS" : "FAIL") << '\n' << log;
        failures += !ok;
        glDeleteProgram(program);
    };
    checkProgram("vertexSource", "fragmentSource");
    checkProgram("organismVertex", "organismFragment");
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(context);
    return failures == 0 ? 0 : 1;
}
