#include "runtime.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <libpng16/png.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// Experimental iOS graphics bridge. Nothing here draws substitute game content.
// The only host draw copies the guest color attachment to the fbdev backbuffer.
namespace {
template<class T> T symbol(void* handle, const char* name) {
    void* p = dlsym(handle, name);
    if (!p) { std::fprintf(stderr,"[graphics] missing native symbol %s\n",name); rt_fail(name); }
    return reinterpret_cast<T>(p);
}
void *mali=nullptr,*egl_lib=nullptr,*gles_lib=nullptr;
#define EGL_API(name) static decltype(&name) p_##name=nullptr
EGL_API(eglGetDisplay); EGL_API(eglInitialize); EGL_API(eglChooseConfig);
EGL_API(eglBindAPI); EGL_API(eglCreateContext); EGL_API(eglCreateWindowSurface);
EGL_API(eglMakeCurrent); EGL_API(eglSwapBuffers); EGL_API(eglGetError);
EGL_API(eglSwapInterval); EGL_API(eglQuerySurface); EGL_API(eglGetProcAddress);
#define GL_API(name) static decltype(&name) p_##name=nullptr
GL_API(glGetString); GL_API(glGetError); GL_API(glGetIntegerv);
GL_API(glGetBooleanv); GL_API(glGetFloatv); GL_API(glIsEnabled);
GL_API(glEnable); GL_API(glDisable); GL_API(glViewport); GL_API(glScissor);
GL_API(glBindFramebuffer); GL_API(glBindRenderbuffer); GL_API(glRenderbufferStorage);
GL_API(glGetRenderbufferParameteriv); GL_API(glGetFramebufferAttachmentParameteriv);
GL_API(glCheckFramebufferStatus); GL_API(glReadPixels); GL_API(glPixelStorei);
GL_API(glCreateShader); GL_API(glShaderSource); GL_API(glCompileShader);
GL_API(glGetShaderiv); GL_API(glGetShaderInfoLog); GL_API(glDeleteShader);
GL_API(glCreateProgram); GL_API(glAttachShader); GL_API(glBindAttribLocation);
GL_API(glLinkProgram); GL_API(glGetProgramiv); GL_API(glGetProgramInfoLog);
GL_API(glUseProgram); GL_API(glGetUniformLocation); GL_API(glUniform1i);
GL_API(glGenTextures); GL_API(glActiveTexture); GL_API(glBindTexture);
GL_API(glTexParameteri); GL_API(glCopyTexImage2D); GL_API(glCopyTexSubImage2D);
GL_API(glBindBuffer); GL_API(glVertexAttribPointer); GL_API(glEnableVertexAttribArray);
GL_API(glDisableVertexAttribArray); GL_API(glGetVertexAttribiv);
GL_API(glGetVertexAttribPointerv); GL_API(glDrawArrays); GL_API(glColorMask);
GL_API(glClearColor); GL_API(glClear); GL_API(glFlush);
PFNGLBINDVERTEXARRAYOESPROC bind_vao=nullptr;
void libraries() {
    if(egl_lib) return;
    // Names have no slash: only the device system loader paths are used.
    mali=dlopen("libMali.so",RTLD_NOW|RTLD_LOCAL);
    if(!mali) mali=dlopen("libMali.so.1",RTLD_NOW|RTLD_LOCAL);
    egl_lib=mali?mali:dlopen("libEGL.so.1",RTLD_NOW|RTLD_LOCAL);
    gles_lib=mali?mali:dlopen("libGLESv2.so.2",RTLD_NOW|RTLD_LOCAL);
    if(!egl_lib||!gles_lib) rt_fail("system fbdev EGL/GLES2 libraries unavailable");
#define LOAD_EGL(n) p_##n=symbol<decltype(p_##n)>(egl_lib,#n)
    LOAD_EGL(eglGetDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglBindAPI); LOAD_EGL(eglCreateContext); LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglSwapBuffers); LOAD_EGL(eglGetError);
    LOAD_EGL(eglSwapInterval); LOAD_EGL(eglQuerySurface); LOAD_EGL(eglGetProcAddress);
#define LOAD_GL(n) p_##n=symbol<decltype(p_##n)>(gles_lib,#n)
    LOAD_GL(glGetString); LOAD_GL(glGetError); LOAD_GL(glGetIntegerv);
    LOAD_GL(glGetBooleanv); LOAD_GL(glGetFloatv); LOAD_GL(glIsEnabled);
    LOAD_GL(glEnable); LOAD_GL(glDisable); LOAD_GL(glViewport); LOAD_GL(glScissor);
    LOAD_GL(glBindFramebuffer); LOAD_GL(glBindRenderbuffer); LOAD_GL(glRenderbufferStorage);
    LOAD_GL(glGetRenderbufferParameteriv); LOAD_GL(glGetFramebufferAttachmentParameteriv);
    LOAD_GL(glCheckFramebufferStatus); LOAD_GL(glReadPixels); LOAD_GL(glPixelStorei);
    LOAD_GL(glCreateShader); LOAD_GL(glShaderSource); LOAD_GL(glCompileShader);
    LOAD_GL(glGetShaderiv); LOAD_GL(glGetShaderInfoLog); LOAD_GL(glDeleteShader);
    LOAD_GL(glCreateProgram); LOAD_GL(glAttachShader); LOAD_GL(glBindAttribLocation);
    LOAD_GL(glLinkProgram); LOAD_GL(glGetProgramiv); LOAD_GL(glGetProgramInfoLog);
    LOAD_GL(glUseProgram); LOAD_GL(glGetUniformLocation); LOAD_GL(glUniform1i);
    LOAD_GL(glGenTextures); LOAD_GL(glActiveTexture); LOAD_GL(glBindTexture);
    LOAD_GL(glTexParameteri); LOAD_GL(glCopyTexImage2D); LOAD_GL(glCopyTexSubImage2D);
    LOAD_GL(glBindBuffer); LOAD_GL(glVertexAttribPointer); LOAD_GL(glEnableVertexAttribArray);
    LOAD_GL(glDisableVertexAttribArray); LOAD_GL(glGetVertexAttribiv);
    LOAD_GL(glGetVertexAttribPointerv); LOAD_GL(glDrawArrays); LOAD_GL(glColorMask);
    LOAD_GL(glClearColor); LOAD_GL(glClear); LOAD_GL(glFlush);
    bind_vao=reinterpret_cast<PFNGLBINDVERTEXARRAYOESPROC>(p_eglGetProcAddress("glBindVertexArrayOES"));
}
int screen_w=0,screen_h=0;
void screen_size() {
    if(screen_w) return;
    int fd=open("/dev/fb0",O_RDONLY|O_CLOEXEC);
    fb_var_screeninfo info{};
    if(fd<0||ioctl(fd,FBIOGET_VSCREENINFO,&info)<0) {
        if(fd>=0) close(fd);
        rt_fail("cannot inspect physical /dev/fb0 dimensions");
    }
    close(fd);
    if(!info.xres||!info.yres||info.xres>8192||info.yres>8192) rt_fail("invalid fbdev dimensions");
    screen_w=info.xres; screen_h=info.yres;
    std::fprintf(stderr,"[graphics] physical fb0 %dx%d %u bpp\n",screen_w,screen_h,info.bits_per_pixel);
}
// Compatibility profile selected from the game's explicit 568-point iPhone
// layout and its native enableRetinaDisplay:YES path; physical EGL stays fb0.
constexpr int logical_w=568,logical_h=320;
constexpr double screen_density=2;
Rect full_rect() {screen_size();return {{0,0},{logical_w,logical_h}};}
double retina_scale(Obj,Sel){return screen_density;}
struct Context {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    EGLConfig config=nullptr;
    struct {unsigned short width,height;} window{};
    GLuint renderbuffer=0,framebuffer=0,texture=0,program=0;
    std::unordered_map<GLuint,GLuint> renderbuffer_fbos;
    int width=0,height=0,copy_w=0,copy_h=0;
    unsigned long frame=0,black=0;
    std::chrono::steady_clock::time_point rate_start{};
    double copy_ms=0,readback_ms=0,scan_ms=0;
    bool saw_nonblack=false;
    unsigned sample_row_index=0;
    std::vector<unsigned char> proof_pixels, sample_pixels;
    std::string proof_directory;
};
thread_local Obj current_context=nullptr;
Context& context(Obj obj) {
    if(!obj||!rt_host(obj).native) rt_fail("EAGL context was not initialized");
    return *static_cast<Context*>(rt_host(obj).native);
}
Obj init_context(Obj obj,Sel,long api,Obj group) {
    if(api!=2) rt_fail("guest requested unsupported EAGL API (only GLES2 is implemented)");
    libraries();screen_size();
    auto* c=new Context; c->window={static_cast<unsigned short>(screen_w),static_cast<unsigned short>(screen_h)};
    c->display=p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major=0,minor=0;
    if(c->display==EGL_NO_DISPLAY||!p_eglInitialize(c->display,&major,&minor)||!p_eglBindAPI(EGL_OPENGL_ES_API))
        rt_fail("fbdev EGL initialization failed");
    EGLint config_attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_NONE};
    EGLint count=0;
    if(!p_eglChooseConfig(c->display,config_attrs,&c->config,1,&count)||count!=1) rt_fail("no EGL GLES2 window config");
    EGLContext shared=EGL_NO_CONTEXT;
    if(group) {Obj owner=rt_host(group).target;if(!owner)rt_fail("unknown EAGL sharegroup");shared=context(owner).context;}
    EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    c->context=p_eglCreateContext(c->display,c->config,shared,attrs);
    c->surface=p_eglCreateWindowSurface(c->display,c->config,reinterpret_cast<EGLNativeWindowType>(&c->window),nullptr);
    if(c->context==EGL_NO_CONTEXT||c->surface==EGL_NO_SURFACE) rt_fail("creating physical EGL GLES2 context/window failed");
    rt_host(obj).native=c;
    std::fprintf(stderr,"[graphics] guest EAGL API=2 created native EGL %d.%d\n",major,minor);
    return obj;
}
Obj init_context_simple(Obj o,Sel s,long api){return init_context(o,s,api,nullptr);}
bool set_context(Obj,Sel,Obj o) {
    if(!o) {if(current_context){auto& c=context(current_context);if(!p_eglMakeCurrent(c.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT))rt_fail("eglMakeCurrent detach failed");}current_context=nullptr;return true;}
    auto& c=context(o);
    if(!p_eglMakeCurrent(c.display,c.surface,c.surface,c.context))rt_fail("eglMakeCurrent failed");
    current_context=o; p_eglSwapInterval(c.display,1);
    static bool logged=false;
    if(!logged){std::fprintf(stderr,"[graphics] GL_VERSION=%s GL_RENDERER=%s\n",p_glGetString(GL_VERSION),p_glGetString(GL_RENDERER));logged=true;}
    return true;
}
Obj get_context(Obj,Sel){return current_context;}
Obj sharegroup(Obj o,Sel){Obj& x=rt_host(o).dictionary["sharegroup"];if(!x){x=rt_new("EAGLSharegroup");rt_host(x).target=o;}return x;}
void track_framebuffer_renderbuffer(GLenum target,GLenum attachment,GLenum rbtarget,GLuint rb) {
    libraries();
    auto native=symbol<decltype(&glFramebufferRenderbuffer)>(gles_lib,"glFramebufferRenderbuffer");
    native(target,attachment,rbtarget,rb);
    if(current_context&&attachment==GL_COLOR_ATTACHMENT0&&rb){auto& c=context(current_context);GLint f=0;p_glGetIntegerv(GL_FRAMEBUFFER_BINDING,&f);c.renderbuffer_fbos[rb]=f;if(rb==c.renderbuffer)c.framebuffer=f;}
}
bool renderbuffer_storage(Obj o,Sel,unsigned target,Obj layer) {
    auto& c=context(o); if(current_context!=o) rt_fail("renderbufferStorage without current EAGL context");
    if(target!=GL_RENDERBUFFER)rt_fail("unexpected EAGL renderbuffer target");
    GLint rb=0; p_glGetIntegerv(GL_RENDERBUFFER_BINDING,&rb); if(!rb)rt_fail("drawable storage requires a bound guest renderbuffer");
    Rect bounds=send<Rect>(layer,"bounds"); double scale=send<double>(layer,"contentsScale");
    if(scale<=0)scale=1;
    c.width=std::lround(bounds.size.width*scale);c.height=std::lround(bounds.size.height*scale);
    if(c.width<=0||c.height<=0||c.width>8192||c.height>8192)rt_fail("invalid drawable renderbuffer size");
    Obj props=send<Obj>(layer,"drawableProperties");
    const char* format=nullptr;
    if(props){Obj v=send<Obj>(props,"objectForKey:",rt_string("EAGLDrawablePropertyColorFormat"));if(v)format=rt_utf8(v);}
    GLenum internal=(format&&std::strstr(format,"565"))?GL_RGB565:GL_RGBA4;
    p_glRenderbufferStorage(target,internal,c.width,c.height);c.renderbuffer=rb;
    c.framebuffer=c.renderbuffer_fbos[rb];
    GLenum err=p_glGetError();if(err!=GL_NO_ERROR)rt_fail("drawable glRenderbufferStorage failed");
    std::fprintf(stderr,"[graphics] guest drawable renderbuffer=%d size=%dx%d format=0x%x\n",rb,c.width,c.height,internal);
    return true;
}
struct Attrib {GLint enabled,size,type,normalized,stride,buffer;void* pointer;};
struct State {
    GLint framebuffer,renderbuffer,program,array_buffer,active_texture,texture,viewport[4],scissor[4],vao=0;
    GLboolean color[4]; GLfloat clear_color[4]; std::array<GLboolean,5> enabled; Attrib attrib[2]; bool use_vao=false;
    static constexpr GLenum caps[5]={GL_BLEND,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_CULL_FACE};
    State(){
        p_glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);p_glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
        p_glGetIntegerv(GL_CURRENT_PROGRAM,&program);p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array_buffer);
        p_glGetIntegerv(GL_ACTIVE_TEXTURE,&active_texture);p_glActiveTexture(GL_TEXTURE0);p_glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        p_glGetIntegerv(GL_VIEWPORT,viewport);p_glGetIntegerv(GL_SCISSOR_BOX,scissor);p_glGetBooleanv(GL_COLOR_WRITEMASK,color);p_glGetFloatv(GL_COLOR_CLEAR_VALUE,clear_color);
        for(unsigned i=0;i<enabled.size();i++)enabled[i]=p_glIsEnabled(caps[i]);
        const char* ext=reinterpret_cast<const char*>(p_glGetString(GL_EXTENSIONS));
        use_vao=bind_vao&&ext&&std::strstr(ext,"GL_OES_vertex_array_object");
        if(use_vao){p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES,&vao);bind_vao(0);}
        for(unsigned i=0;i<2;i++){
            auto& a=attrib[i];p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&a.enabled);
            p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_SIZE,&a.size);p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_TYPE,&a.type);
            p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&a.normalized);p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&a.stride);
            p_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&a.buffer);p_glGetVertexAttribPointerv(i,GL_VERTEX_ATTRIB_ARRAY_POINTER,&a.pointer);
        }
    }
    ~State(){
        for(unsigned i=0;i<2;i++){auto& a=attrib[i];p_glBindBuffer(GL_ARRAY_BUFFER,a.buffer);p_glVertexAttribPointer(i,a.size,a.type,a.normalized,a.stride,a.pointer);if(a.enabled)p_glEnableVertexAttribArray(i);else p_glDisableVertexAttribArray(i);}
        if(use_vao)bind_vao(vao);
        p_glBindBuffer(GL_ARRAY_BUFFER,array_buffer);p_glUseProgram(program);
        p_glBindTexture(GL_TEXTURE_2D,texture);p_glActiveTexture(active_texture);
        p_glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);p_glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
        p_glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);p_glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
        p_glColorMask(color[0],color[1],color[2],color[3]);
        p_glClearColor(clear_color[0],clear_color[1],clear_color[2],clear_color[3]);
        for(unsigned i=0;i<enabled.size();i++){if(enabled[i])p_glEnable(caps[i]);else p_glDisable(caps[i]);}
    }
};
GLuint shader(GLenum type,const char* source){
    GLuint s=p_glCreateShader(type);p_glShaderSource(s,1,&source,nullptr);p_glCompileShader(s);GLint ok=0;p_glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048]{};p_glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"[graphics] copy shader: %s\n",log);rt_fail("GLES2 framebuffer copy shader failed");}return s;
}
void guest_shader_source(GLuint shader_id,GLsizei count,const GLchar*const* strings,const GLint* lengths){
    // The observed cocos2d round-point shader appends its unconditional
    // derivative directive after the CC uniform prelude. Mali rejects that
    // ordering. Move only this identified directive ahead of that prelude.
    if((count==3||count==4)&&strings){
        std::string combined;
        for(GLsizei i=0;i<count;++i){
            if(!strings[i])rt_fail("NULL guest shader source segment");
            size_t n=lengths&&lengths[i]>=0?size_t(lengths[i]):strlen(strings[i]);
            if(n>1024*1024)rt_fail("guest shader source size limit");
            combined.append(strings[i],n);
        }
        const std::string directive="#extension GL_OES_standard_derivatives : enable";
        auto marker=combined.find("//CC INCLUDES END\n");
        auto extension=combined.find(directive);
        if(combined.rfind("precision mediump float;\n",0)==0&&marker!=std::string::npos&&extension!=std::string::npos&&extension>marker){
            size_t prefix_end=marker+strlen("//CC INCLUDES END\n");
            bool whitespace=true;for(size_t i=prefix_end;i<extension;++i)whitespace &= combined[i]==' '||combined[i]=='\t'||combined[i]=='\r'||combined[i]=='\n';
            auto end=combined.find('\n',extension);
            if(whitespace&&end!=std::string::npos){
                std::string line=combined.substr(extension,end-extension+1);
                combined.erase(extension,end-extension+1);combined.insert(0,line);
                const GLchar* adjusted=combined.c_str();GLint size=combined.size();
                p_glShaderSource(shader_id,1,&adjusted,&size);
                std::fprintf(stderr,"SHADER_ADAPTER id=%u moved unconditional derivatives directive before cocos2d uniform prelude\n",shader_id);
                return;
            }
        }
    }
    p_glShaderSource(shader_id,count,strings,lengths);
}
void compile_guest_shader(GLuint shader_id){
    p_glCompileShader(shader_id);
    GLint status=0;p_glGetShaderiv(shader_id,GL_COMPILE_STATUS,&status);
    if(status)return;
    char info[8192]{};p_glGetShaderInfoLog(shader_id,sizeof(info),nullptr,info);
    GLint length=0;p_glGetShaderiv(shader_id,GL_SHADER_SOURCE_LENGTH,&length);
    std::fprintf(stderr,"GUEST_SHADER_COMPILE_FAILED id=%u log=%s\n",shader_id,info);
    if(length>0&&length<1024*1024){
        std::vector<char> source(size_t(length)+1);
        auto get_source=symbol<decltype(&glGetShaderSource)>(gles_lib,"glGetShaderSource");
        get_source(shader_id,length,nullptr,source.data());
        std::fprintf(stderr,"GUEST_SHADER_SOURCE_BEGIN\n%s\nGUEST_SHADER_SOURCE_END\n",source.data());
    }
}
void copy_to_backbuffer(Context& c) {
    if(!c.framebuffer)rt_fail("cannot identify guest drawable FBO; refusing substitute frame");
    p_glBindFramebuffer(GL_FRAMEBUFFER,c.framebuffer);
    if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)rt_fail("guest drawable FBO incomplete");
    if(!c.texture){p_glGenTextures(1,&c.texture);}
    p_glActiveTexture(GL_TEXTURE0);p_glBindTexture(GL_TEXTURE_2D,c.texture);
    p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    // RGB565 drawable cannot supply an RGBA copy texture in the GLES2 contract.
    if(c.copy_w!=c.width||c.copy_h!=c.height){p_glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGB,0,0,c.width,c.height,0);c.copy_w=c.width;c.copy_h=c.height;}
    else p_glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,c.width,c.height);
    if(!c.program){
        GLuint vs=shader(GL_VERTEX_SHADER,"attribute vec2 a_position;attribute vec2 a_uv;varying vec2 uv;void main(){uv=a_uv;gl_Position=vec4(a_position,0.0,1.0);}");
        // Mali fbdev's OSD alpha blending requires opaque backbuffer alpha. RGB is unchanged.
        GLuint fs=shader(GL_FRAGMENT_SHADER,"precision mediump float;uniform sampler2D image;varying vec2 uv;void main(){gl_FragColor=vec4(texture2D(image,uv).rgb,1.0);}");
        c.program=p_glCreateProgram();p_glAttachShader(c.program,vs);p_glAttachShader(c.program,fs);p_glBindAttribLocation(c.program,0,"a_position");p_glBindAttribLocation(c.program,1,"a_uv");p_glLinkProgram(c.program);
        GLint ok=0;p_glGetProgramiv(c.program,GL_LINK_STATUS,&ok);if(!ok)rt_fail("GLES2 framebuffer copy program failed");p_glDeleteShader(vs);p_glDeleteShader(fs);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER,0);for(GLenum cap:State::caps)p_glDisable(cap);
    p_glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    p_glClearColor(0,0,0,1);p_glClear(GL_COLOR_BUFFER_BIT);
    double factor=std::min(double(screen_w)/c.width,double(screen_h)/c.height);
    int fit_w=std::lround(c.width*factor),fit_h=std::lround(c.height*factor);
    p_glViewport((screen_w-fit_w)/2,(screen_h-fit_h)/2,fit_w,fit_h);
    p_glUseProgram(c.program);p_glUniform1i(p_glGetUniformLocation(c.program,"image"),0);p_glBindBuffer(GL_ARRAY_BUFFER,0);
    static const GLfloat vertices[]={-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices);p_glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices+2);
    p_glEnableVertexAttribArray(0);p_glEnableVertexAttribArray(1);p_glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
void proof_dir(Context& c){
    if(!c.proof_directory.empty())return;
    char path[]="/tmp/goblin-ios-video.XXXXXX";char* p=mkdtemp(path);if(!p)rt_fail("cannot create private visual-evidence directory");
    c.proof_directory=p;std::fprintf(stderr,"[video-proof] private directory=%s\n",p);
}
struct FrameSample {
    const char* mode="full_initial";
    // pixels_tested describes the classification area. pixels_read also counts
    // the rotating row preceding a full fallback, so transfer work is explicit.
    size_t pixels_tested=0,pixels_read=0,nonblack=0;
    int row_y=-1;
};
void receipt(Context& c,const char* result,const FrameSample& sample,GLenum error){
    proof_dir(c);std::string path=c.proof_directory+"/present-"+std::to_string(c.frame)+".json";
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)rt_fail("cannot write exclusive visual receipt");
    dprintf(fd,"{\"scope\":\"prototype-private-not-release-health\",\"pid\":%d,\"frame\":%lu,\"result\":\"%s\",\"source_fbo\":%u,\"sampled_fbo\":0,\"width\":%d,\"height\":%d,\"sample_mode\":\"%s\",\"sample_row_y\":%d,\"pixels_tested\":%zu,\"pixels_read\":%zu,\"nonblack_pixels\":%zu,\"gl_error\":%u}\n",getpid(),c.frame,result,c.framebuffer,screen_w,screen_h,sample.mode,sample.row_y,sample.pixels_tested,sample.pixels_read,sample.nonblack,error);close(fd);
}
FrameSample sample_backbuffer(Context& c){
    using Clock=std::chrono::steady_clock;
    FrameSample sample;
    if(c.saw_nonblack)sample.mode="rotating_row";
    auto setup=Clock::now();
    GLint pack=4;p_glGetIntegerv(GL_PACK_ALIGNMENT,&pack);p_glPixelStorei(GL_PACK_ALIGNMENT,1);
    c.readback_ms+=std::chrono::duration<double,std::milli>(Clock::now()-setup).count();
    GLenum error=GL_NO_ERROR;
    auto read=[&](bool full){
        auto begin=Clock::now();
        auto& pixels=full?c.proof_pixels:c.sample_pixels;
        size_t count=size_t(screen_w)*(full?screen_h:1);
        pixels.resize(count*4);
        if(full){
            p_glReadPixels(0,0,screen_w,screen_h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            sample.pixels_read+=count;error=p_glGetError();
        }else{
            const int rows[]={screen_h/4,screen_h/2,3*screen_h/4};
            sample.row_y=rows[c.sample_row_index];
            c.sample_row_index=(c.sample_row_index+1)%3;
            p_glReadPixels(0,sample.row_y,screen_w,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            sample.pixels_read+=screen_w;error=p_glGetError();
        }
        c.readback_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        if(error)return;
        begin=Clock::now();sample.nonblack=0;sample.pixels_tested=count;
        for(size_t i=0;i<count*4;i+=4)if(pixels[i]>3||pixels[i+1]>3||pixels[i+2]>3)++sample.nonblack;
        c.scan_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    };
    read(!c.saw_nonblack);
    if(!error&&c.saw_nonblack&&!sample.nonblack){
        sample.mode="full_fallback";
        read(true); // A dark sample alone never certifies a black framebuffer.
    }
    auto finish=Clock::now();p_glPixelStorei(GL_PACK_ALIGNMENT,pack);
    GLenum restore_error=p_glGetError();if(!error)error=restore_error;
    c.readback_ms+=std::chrono::duration<double,std::milli>(Clock::now()-finish).count();
    if(error){receipt(c,"DEAD-CONTEXT",sample,error);rt_fail("GL error at physical framebuffer readback/present");}
    return sample;
}
void check_black_streak(Context& c,const FrameSample& sample){
    if(sample.nonblack)c.black=0;
    else if(++c.black>=60){receipt(c,"BLACK",sample,0);rt_fail("60 consecutive black presents; exact prototype process stopping");}
}
bool present(Obj o,Sel,unsigned target){
    auto& c=context(o);if(current_context!=o||target!=GL_RENDERBUFFER)rt_fail("invalid EAGL present context/target");
    ++c.frame;
    using Clock=std::chrono::steady_clock;
    auto begin=Clock::now();if(c.frame==1)c.rate_start=begin;
    State state; copy_to_backbuffer(c);
    auto copied=Clock::now();
    c.copy_ms+=std::chrono::duration<double,std::milli>(copied-begin).count();
    FrameSample sample=sample_backbuffer(c);
    check_black_streak(c,sample);
    if(sample.nonblack){
        if(!c.saw_nonblack){
            c.saw_nonblack=true;receipt(c,"NONBLACK-PIXELS",sample,0);
            std::string path=c.proof_directory+"/first-nonblack.ppm";int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0)rt_fail("cannot write first guest frame evidence");
            dprintf(fd,"P6\n%d %d\n255\n",screen_w,screen_h);
            std::vector<unsigned char> row(size_t(screen_w)*3);
            for(int y=screen_h-1;y>=0;--y){for(int x=0;x<screen_w;x++)std::memcpy(row.data()+x*3,c.proof_pixels.data()+(size_t(y)*screen_w+x)*4,3);size_t off=0;while(off<row.size()){ssize_t n=write(fd,row.data()+off,row.size()-off);if(n<=0)rt_fail("writing pixel evidence failed");off+=n;}}
            close(fd);std::fprintf(stderr,"[video-proof] real guest-derived nonblack pixels=%zu sample_mode=%s pixels_tested=%zu pixels_read=%zu frame=%lu PPM=%s (gameplay not certified)\n",sample.nonblack,sample.mode,sample.pixels_tested,sample.pixels_read,c.frame,path.c_str());
        }
    }
    if(!p_eglSwapBuffers(c.display,c.surface)){receipt(c,"DEAD-CONTEXT",sample,p_eglGetError());rt_fail("eglSwapBuffers failed");}
    if(c.frame%120==0){
        auto now=Clock::now();double seconds=std::chrono::duration<double>(now-c.rate_start).count();
        std::fprintf(stderr,"[graphics] present=%lu nonblack=%zu sample_mode=%s sample_row_y=%d pixels_tested=%zu pixels_read=%zu black_streak=%lu fps=%.2f copy_ms=%.3f readback_ms=%.3f scan_ms=%.3f\n",
                     c.frame,sample.nonblack,sample.mode,sample.row_y,sample.pixels_tested,sample.pixels_read,c.black,120/seconds,c.copy_ms/120,c.readback_ms/120,c.scan_ms/120);
        c.rate_start=now;c.copy_ms=c.readback_ms=c.scan_ms=0;
    }
    return true;
}
// UIKit host state is separate from native guest ivars.
Obj screen=nullptr;
Obj main_screen(Obj,Sel){if(!screen){screen=rt_new("UIScreen");rt_host(screen).frame=full_rect();}return screen;}
Obj screen_mode(Obj o,Sel){Obj& mode=rt_host(o).dictionary["mode"];if(!mode){mode=rt_new("UIScreenMode");rt_host(mode).frame={{0,0},{logical_w*screen_density,logical_h*screen_density}};}return mode;}
Size mode_size(Obj o,Sel){return rt_host(o).frame.size;}
Rect bounds(Obj o,Sel){Rect r=rt_host(o).frame;if(r.size.width<=0||r.size.height<=0)r=full_rect();r.origin={0,0};return r;}
Rect frame(Obj o,Sel){return rt_host(o).frame;}
void set_frame(Obj o,Sel,Rect r){rt_host(o).frame=r;Obj layer=rt_host(o).dictionary["layer"];if(layer)rt_host(layer).frame={{0,0},r.size};}
Obj init_frame(Obj o,Sel,Rect r){set_frame(o,nullptr,r);return o;}
double one(Obj,Sel){return 1;}
double scale(Obj o,Sel){double x=rt_host(o).number;return x>0?x:1;}
void set_scale(Obj o,Sel selector,double n){
    if(!std::isfinite(n)||n<=0)rt_fail("invalid UIKit scale");
    auto& h=rt_host(o);h.number=n;
    // UIKit owns its backing layer's scale; a direct CALayer.contentsScale
    // assignment remains independent of a view's contentScaleFactor.
    if(selector&&!std::strcmp(selector,"setContentScaleFactor:")){
        auto found=h.dictionary.find("layer");
        if(found!=h.dictionary.end()&&found->second)rt_host(found->second).number=n;
    }
}
Obj props(Obj o,Sel){return rt_host(o).dictionary["properties"];}
void set_props(Obj o,Sel,Obj p){rt_host(o).dictionary["properties"]=p;}
Obj layer_class(Obj,Sel){return rt_class("CALayer");}
Obj layer(Obj o,Sel){
    Obj& l=rt_host(o).dictionary["layer"];
    if(!l){Obj cls=send<Obj>(reinterpret_cast<Obj>(*static_cast<void**>(o)),"layerClass");l=send<Obj>(cls,"alloc");l=send<Obj>(l,"init");rt_host(l).frame={{0,0},bounds(o,nullptr).size};rt_host(l).number=scale(o,nullptr);}
    return l;
}
void noarg(Obj,Sel){}
void bool_arg(Obj,Sel,bool){}
void integer_arg(Obj,Sel,long){}
void obj_arg(Obj,Sel,Obj){}
void two_obj_arg(Obj,Sel,Obj,Obj){}
bool unavailable_keyboard_focus(Obj o,Sel){std::fprintf(stderr,"[UIKit] keyboard first-responder unavailable for %s; returning false\n",rt_class_name(o));return false;}
bool no_first_responder(Obj,Sel){return false;}
bool resign_unused_responder(Obj,Sel){return true;}
Obj get_view(Obj o,Sel){Obj& v=rt_host(o).dictionary["view"];if(!v){v=rt_new("UIView");rt_host(v).frame=full_rect();}return v;}
void set_view(Obj o,Sel,Obj v){rt_host(o).dictionary["view"]=v;}
Obj init_root(Obj o,Sel,Obj root){rt_host(o).dictionary["root"]=root;return o;}
Obj root(Obj o,Sel){return rt_host(o).dictionary["root"];}
void set_root(Obj o,Sel,Obj r){rt_host(o).dictionary["root"]=r;}
void set_delegate(Obj o,Sel,Obj d){rt_host(o).dictionary["delegate"]=d;}
Obj get_delegate(Obj o,Sel){return rt_host(o).dictionary["delegate"];}
void add_subview(Obj o,Sel,Obj v){rt_host(o).array.push_back(v);rt_host(v).dictionary["superview"]=o;}
Obj superview(Obj o,Sel){return rt_host(o).dictionary["superview"];}
Obj nav_view(Obj o,Sel){Obj r=root(o,nullptr);return r?send<Obj>(r,"view"):get_view(o,nullptr);}
void optional(Obj o,Sel selector,bool flag){if(o&&send<bool>(o,"respondsToSelector:",selector))send<void>(o,selector,flag);}
void layout_tree(Obj view){
    if(!view)return;
    send<void>(view,"layoutSubviews");
    auto children=rt_host(view).array;for(Obj child:children)layout_tree(child);
}
void show_window(Obj window,Sel){
    auto& h=rt_host(window);if(h.dictionary["shown"])return;h.dictionary["shown"]=window;
    Obj controller=h.dictionary["root"];
    std::vector<Obj> controllers;
    while(controller){controllers.push_back(controller);Obj next=rt_host(controller).dictionary["root"];if(next==controller)rt_fail("UIKit controller cycle");controller=next;}
    for(Obj c:controllers)optional(c,"viewWillAppear:",false);
    if(!controllers.empty()){
        Obj v=send<Obj>(controllers.back(),"view");
        Rect r=full_rect();send<void>(v,"setFrame:",r);layout_tree(v);
    } else layout_tree(window);
    for(auto it=controllers.rbegin();it!=controllers.rend();++it)optional(*it,"viewDidAppear:",false);
    std::fprintf(stderr,"[UIKit] native view appearance/layout callbacks delivered\n");
}
struct DisplayLink {Obj object,target;Sel selector;long interval=1;bool active=false,paused=false;double timestamp=0;unsigned long ticks=0;};
std::unordered_map<Obj,DisplayLink> links;
double media_time(){using C=std::chrono::steady_clock;return std::chrono::duration<double>(C::now().time_since_epoch()).count();}
Obj new_link(Obj,Sel,Obj target,Sel selector){Obj o=rt_new("CADisplayLink");links[o]={o,target,selector};return o;}
void add_link(Obj o,Sel,Obj,Obj){links.at(o).active=true;}
void invalidate_link(Obj o,Sel){links.at(o).active=false;}
void pause_link(Obj o,Sel,bool p){links.at(o).paused=p;}
void interval_link(Obj o,Sel,long n){if(n<1||n>120)rt_fail("invalid CADisplayLink interval");links.at(o).interval=n;}
double link_time(Obj o,Sel){return links.at(o).timestamp;}
double link_duration(Obj,Sel){return 1.0/60.0;}
// CoreGraphics geometry uses iOS CGFloat=double and real HFA return signatures.
struct Affine {double a,b,c,d,tx,ty;};
const Point point_zero{0,0};const Size size_zero{0,0};const Rect rect_zero{{0,0},{0,0}};
const Affine affine_identity{1,0,0,1,0,0};
Point transform_point(Point p,Affine t){return {p.x*t.a+p.y*t.c+t.tx,p.x*t.b+p.y*t.d+t.ty};}
Affine concat(Affine t,Affine s){return {t.a*s.a+t.b*s.c,t.a*s.b+t.b*s.d,t.c*s.a+t.d*s.c,t.c*s.b+t.d*s.d,t.tx*s.a+t.ty*s.c+s.tx,t.tx*s.b+t.ty*s.d+s.ty};}
Affine invert(Affine t){double det=t.a*t.d-t.b*t.c;if(det==0)return t;return {t.d/det,-t.b/det,-t.c/det,t.a/det,(t.c*t.ty-t.d*t.tx)/det,(t.b*t.tx-t.a*t.ty)/det};}
Affine translate(Affine t,double x,double y){t.tx+=x*t.a+y*t.c;t.ty+=x*t.b+y*t.d;return t;}
Rect standardized(Rect r){if(r.size.width<0){r.origin.x+=r.size.width;r.size.width=-r.size.width;}if(r.size.height<0){r.origin.y+=r.size.height;r.size.height=-r.size.height;}return r;}
bool rect_null(Rect r){return std::isinf(r.origin.x)&&std::isinf(r.origin.y);}
bool rect_contains(Rect r,Point p){if(rect_null(r))return false;r=standardized(r);return p.x>=r.origin.x&&p.y>=r.origin.y&&p.x<r.origin.x+r.size.width&&p.y<r.origin.y+r.size.height;}
bool rect_equal(Rect a,Rect b){return a.origin.x==b.origin.x&&a.origin.y==b.origin.y&&a.size.width==b.size.width&&a.size.height==b.size.height;}
bool rect_intersects(Rect a,Rect b){if(rect_null(a)||rect_null(b))return false;a=standardized(a);b=standardized(b);return a.size.width>0&&a.size.height>0&&b.size.width>0&&b.size.height>0&&std::max(a.origin.x,b.origin.x)<std::min(a.origin.x+a.size.width,b.origin.x+b.size.width)&&std::max(a.origin.y,b.origin.y)<std::min(a.origin.y+a.size.height,b.origin.y+b.size.height);}
Rect rect_transform(Rect r,Affine t){Point p[4]={{r.origin.x,r.origin.y},{r.origin.x+r.size.width,r.origin.y},{r.origin.x,r.origin.y+r.size.height},{r.origin.x+r.size.width,r.origin.y+r.size.height}};double x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;for(auto a:p){auto b=transform_point(a,t);x0=std::min(x0,b.x);x1=std::max(x1,b.x);y0=std::min(y0,b.y);y1=std::max(y1,b.y);}return {{x0,y0},{x1-x0,y1-y0}};}
std::vector<double> parse_numbers(Obj s){const char* p=rt_utf8(s);std::vector<double> v;while(p&&*p){char* end=nullptr;double x=std::strtod(p,&end);if(end!=p){v.push_back(x);p=end;}else ++p;}return v;}
Point point_string(Obj s){auto v=parse_numbers(s);return v.size()==2?Point{v[0],v[1]}:point_zero;}
Size size_string(Obj s){auto v=parse_numbers(s);return v.size()==2?Size{v[0],v[1]}:size_zero;}
Rect rect_string(Obj s){auto v=parse_numbers(s);return v.size()==4?Rect{{v[0],v[1]},{v[2],v[3]}}:rect_zero;}
struct ColorSpace {int channels;};ColorSpace rgb_space{3},gray_space{1};
void* color_rgb(){return &rgb_space;}void* color_gray(){return &gray_space;}void color_release(void*){}
struct Provider {std::vector<unsigned char> bytes;void* info=nullptr;const void* original=nullptr;size_t size=0;void(*release)(void*,const void*,size_t)=nullptr;};
void* provider_data(void* info,const void* data,size_t count,void(*release)(void*,const void*,size_t)){
    if((!data&&count)||count>512UL*1024*1024)rt_fail("invalid CGDataProvider bytes");
    auto* p=new Provider;if(count)p->bytes.assign(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+count);p->info=info;p->original=data;p->size=count;p->release=release;return p;
}
void* provider_cf(Obj data){return provider_data(nullptr,send<const void*>(data,"bytes"),send<size_t>(data,"length"),nullptr);}
void provider_release(Provider* p){if(!p)return;if(p->release)p->release(p->info,p->original,p->size);delete p;}
struct Image {size_t width=0,height=0;std::vector<unsigned char> rgba;int references=1;};
uint32_t be32(const unsigned char* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
int paeth(int a,int b,int c){int p=a+b-c,pa=std::abs(p-a),pb=std::abs(p-b),pc=std::abs(p-c);return pa<=pb&&pa<=pc?a:pb<=pc?b:c;}
Image* decode_cgbi(Provider* provider){
    const auto& bytes=provider->bytes;std::vector<unsigned char> compressed;
    size_t width=0,height=0;unsigned channels=0;bool ihdr=false,iend=false;
    for(size_t offset=8;offset+12<=bytes.size();){
        uint32_t n=be32(bytes.data()+offset);if(n>bytes.size()-offset-12)rt_fail("truncated CgBI PNG chunk");
        const auto* type=bytes.data()+offset+4;const auto* data=bytes.data()+offset+8;
        if(std::memcmp(type,"IHDR",4)==0){
            if(n!=13||ihdr)rt_fail("invalid CgBI IHDR");
            ihdr=true;width=be32(data);height=be32(data+4);
            if(!width||!height||width>16384||height>16384||data[8]!=8||(data[9]!=2&&data[9]!=6)||data[10]||data[11]||data[12])rt_fail("unsupported CgBI pixel layout/interlace");
            channels=data[9]==6?4:3;
        } else if(std::memcmp(type,"IDAT",4)==0)compressed.insert(compressed.end(),data,data+n);
        else if(std::memcmp(type,"IEND",4)==0){iend=true;break;}
        offset+=n+12;
    }
    if(!ihdr||!iend||compressed.empty())rt_fail("incomplete CgBI PNG");
    size_t stride=width*channels,total=(stride+1)*height;if(total>256UL*1024*1024)rt_fail("CgBI image exceeds prototype decode bound");
    static void* lib=dlopen("libz.so.1",RTLD_NOW|RTLD_LOCAL);if(!lib)rt_fail("system zlib unavailable for CgBI PNG");
    static auto init=symbol<decltype(&inflateInit2_)>(lib,"inflateInit2_");
    static auto inflate_data=symbol<decltype(&inflate)>(lib,"inflate");
    static auto finish=symbol<decltype(&inflateEnd)>(lib,"inflateEnd");
    std::vector<unsigned char> filtered(total),raw(stride*height);z_stream stream{};
    stream.next_in=const_cast<Bytef*>(compressed.data());stream.avail_in=compressed.size();stream.next_out=filtered.data();stream.avail_out=filtered.size();
    if(init(&stream,-MAX_WBITS,ZLIB_VERSION,sizeof(stream))!=Z_OK)rt_fail("CgBI raw-deflate initialization failed");
    int status=inflate_data(&stream,Z_FINISH);size_t produced=stream.total_out;finish(&stream);
    if(status!=Z_STREAM_END||produced!=total)rt_fail("CgBI raw-deflate payload mismatch");
    for(size_t y=0;y<height;y++){
        const auto* src=filtered.data()+y*(stride+1)+1;auto* dst=raw.data()+y*stride;unsigned filter=filtered[y*(stride+1)];
        if(filter>4)rt_fail("unknown CgBI PNG row filter");
        for(size_t x=0;x<stride;x++){
            unsigned a=x>=channels?dst[x-channels]:0,b=y?raw[(y-1)*stride+x]:0,c=y&&x>=channels?raw[(y-1)*stride+x-channels]:0;
            unsigned predict=filter==1?a:filter==2?b:filter==3?(a+b)/2:filter==4?paeth(a,b,c):0;dst[x]=src[x]+predict;
        }
    }
    auto* image=new Image;image->width=width;image->height=height;image->rgba.resize(width*height*4);
    for(size_t i=0;i<width*height;i++){
        const auto* src=raw.data()+i*channels;auto* dst=image->rgba.data()+i*4;unsigned alpha=channels==4?src[3]:255;
        // Apple's CgBI stores BGR(A), premultiplied when alpha is present.
        for(unsigned c=0;c<3;c++)dst[c]=alpha?std::min(255u,(unsigned(src[2-c])*255+alpha/2)/alpha):0;
        dst[3]=alpha;
    }
    return image;
}
Image* decode_png(Provider* p,const double*,bool,long){
    if(!p)rt_fail("null PNG provider");
    if(p->bytes.size()>=16&&std::memcmp(p->bytes.data()+12,"CgBI",4)==0)return decode_cgbi(p);
    static void* lib=dlopen("libpng16.so.16",RTLD_NOW|RTLD_LOCAL);if(!lib)rt_fail("system libpng16 unavailable");
    static auto begin=symbol<decltype(&png_image_begin_read_from_memory)>(lib,"png_image_begin_read_from_memory");
    static auto finish=symbol<decltype(&png_image_finish_read)>(lib,"png_image_finish_read");
    static auto free_image=symbol<decltype(&png_image_free)>(lib,"png_image_free");
    png_image image{};image.version=PNG_IMAGE_VERSION;
    if(!begin(&image,p->bytes.data(),p->bytes.size())){std::fprintf(stderr,"[PNG] %s\n",image.message);rt_fail("PNG decode header failed");}
    image.format=PNG_FORMAT_RGBA;if(!image.width||!image.height||image.width>16384||image.height>16384)rt_fail("PNG dimensions unsupported");
    auto* result=new Image;result->width=image.width;result->height=image.height;result->rgba.resize(PNG_IMAGE_SIZE(image));
    if(!finish(&image,nullptr,result->rgba.data(),0,nullptr)){std::fprintf(stderr,"[PNG] %s\n",image.message);rt_fail("PNG pixel decode failed");}free_image(&image);return result;
}
size_t image_width(Image* i){return i?i->width:0;}size_t image_height(Image* i){return i?i->height:0;}
size_t image_bpc(Image*){return 8;}size_t image_row(Image* i){return i?i->width*4:0;}void* image_space(Image*){return &rgb_space;}unsigned image_alpha(Image*){return 3;}
void image_release(Image* i){if(i&&!--i->references)delete i;}
Image* image_create(size_t w,size_t h,size_t bpc,size_t bpp,size_t row,ColorSpace* cs,unsigned info,Provider* p,const double*,bool,long){
    if(!p||!cs||bpc!=8||bpp!=32||cs->channels!=3||row<w*4||w>16384||h>16384||row*h>p->bytes.size())rt_fail("unsupported CGImageCreate layout");
    unsigned alpha=info&31;if(alpha!=1&&alpha!=3&&alpha!=5)rt_fail("unsupported CGImageCreate alpha layout");
    auto* image=new Image;image->width=w;image->height=h;image->rgba.resize(w*h*4);
    for(size_t y=0;y<h;y++)std::memcpy(image->rgba.data()+y*w*4,p->bytes.data()+y*row,w*4);
    return image;
}
struct Bitmap {unsigned char* data=nullptr;std::vector<unsigned char> owned;size_t w,h,row;unsigned channels,info;Affine transform=affine_identity;};
Bitmap* bitmap_create(void* data,size_t w,size_t h,size_t bpc,size_t row,ColorSpace* color,unsigned info){
    if(!w||!h||w>16384||h>16384||bpc!=8)rt_fail("unsupported CGBitmapContext dimensions/component format");
    unsigned channels=color&&color->channels==3?4:1;if(!row)row=w*channels;if(row<w*channels)rt_fail("short CGBitmapContext row");
    unsigned alpha=info&31;if(channels==4&&alpha!=1&&alpha!=3&&alpha!=5)rt_fail("unsupported CGBitmapContext alpha position");
    unsigned byte_order=info&0x7000;
    // CCTexture2D at 0x100084b98 explicitly requests 32Big|alphaLast.
    // Big-endian component order is RGBA/RGBX in memory even on ARM64 LE;
    // bitmap_draw writes components individually and already preserves it.
    if(byte_order!=0&&!(channels==4&&byte_order==0x4000)){
        std::fprintf(stderr,"[CG] unsupported bitmap w=%zu h=%zu bpc=%zu row=%zu channels=%u flags=0x%x\n",w,h,bpc,row,channels,info);
        rt_fail("unsupported explicit CGBitmap byte order");
    }
    static unsigned bitmap_logs=0;
    if(bitmap_logs++<16)std::fprintf(stderr,"[CG] bitmap w=%zu h=%zu bpc=%zu row=%zu channels=%u flags=0x%x order=0x%x alpha=%u\n",w,h,bpc,row,channels,info,byte_order,alpha);
    auto* b=new Bitmap;b->w=w;b->h=h;b->row=row;b->channels=channels;b->info=info;
    if(data)b->data=static_cast<unsigned char*>(data);else {b->owned.resize(row*h);b->data=b->owned.data();}return b;
}
void bitmap_release(Bitmap* b){delete b;}
void bitmap_scale(Bitmap* b,double x,double y){b->transform.a*=x;b->transform.b*=x;b->transform.c*=y;b->transform.d*=y;}
void bitmap_translate(Bitmap* b,double x,double y){b->transform=translate(b->transform,x,y);}
void bitmap_clear(Bitmap* b,Rect r){Affine inv=invert(b->transform);for(size_t y=0;y<b->h;y++)for(size_t x=0;x<b->w;x++)if(rect_contains(r,transform_point({x+.5,y+.5},inv)))std::memset(b->data+y*b->row+x*b->channels,0,b->channels);}
void bitmap_draw(Bitmap* b,Rect r,Image* image){
    if(!b||!image||r.size.width==0||r.size.height==0)rt_fail("invalid CGContextDrawImage input");
    Affine inv=invert(b->transform);
    for(size_t y=0;y<b->h;y++)for(size_t x=0;x<b->w;x++){
        Point q=transform_point({x+.5,y+.5},inv);double u=(q.x-r.origin.x)/r.size.width,v=(q.y-r.origin.y)/r.size.height;if(u<0||u>=1||v<0||v>=1)continue;
        size_t sx=std::min(image->width-1,size_t(u*image->width)),sy=std::min(image->height-1,size_t(v*image->height));
        const auto* src=image->rgba.data()+(sy*image->width+sx)*4;auto* dst=b->data+y*b->row+x*b->channels;
        if(b->channels==1){dst[0]=(b->info&31)==7?src[3]:static_cast<unsigned char>((unsigned(src[0])*77+unsigned(src[1])*150+unsigned(src[2])*29)>>8);}
        else {unsigned alpha=b->info&31;for(unsigned i=0;i<3;i++)dst[i]=alpha==1?static_cast<unsigned char>((unsigned(src[i])*src[3]+127)/255):src[i];dst[3]=alpha==5?255:src[3];}
    }
}
Image* bitmap_image(Bitmap* b){auto* i=new Image;i->width=b->w;i->height=b->h;i->rgba.resize(b->w*b->h*4);for(size_t y=0;y<b->h;y++)for(size_t x=0;x<b->w;x++){auto* dst=i->rgba.data()+(y*b->w+x)*4;const auto* src=b->data+y*b->row+x*b->channels;if(b->channels==4)std::memcpy(dst,src,4);else{dst[0]=dst[1]=dst[2]=src[0];dst[3]=255;}}return i;}
std::vector<Bitmap*> bitmap_stack;
void push_bitmap(Bitmap* b){if(!b)rt_fail("UIGraphicsPushContext null");bitmap_stack.push_back(b);}
void pop_bitmap(){if(bitmap_stack.empty())rt_fail("unbalanced UIGraphicsPopContext");bitmap_stack.pop_back();}
Obj image_file(Obj o,Sel,Obj path){
    std::string name=rt_utf8(path);FILE* f=std::fopen(name.c_str(),"rb");if(!f)return nullptr;
    Provider p;unsigned char buf[65536];size_t n;while((n=std::fread(buf,1,sizeof(buf),f)))p.bytes.insert(p.bytes.end(),buf,buf+n);std::fclose(f);
    if(p.bytes.size()<8||std::memcmp(p.bytes.data(),"\x89PNG\r\n\x1a\n",8))rt_fail("UIImage file decoder currently supports PNG only");
    rt_host(o).native=decode_png(&p,nullptr,false,0);return o;
}
Obj image_named(Obj,Sel,Obj name){std::string path=rt_data_root+"/"+rt_utf8(name);if(path.size()<4||path.substr(path.size()-4)!=".png")path+=".png";return image_file(rt_new("UIImage"),nullptr,rt_string(path.c_str()));}
Image* ui_cgimage(Obj o,Sel){return static_cast<Image*>(rt_host(o).native);}
Size ui_image_size(Obj o,Sel){Image* i=ui_cgimage(o,nullptr);return i?Size{double(i->width),double(i->height)}:size_zero;}
Obj color_object(Obj,Sel){return rt_new("UIColor");}
// Symbols which bind successfully but must stop at their first unsupported use.
[[noreturn]] void unsupported_graphics(){rt_fail("unimplemented CoreGraphics/text/MSAA operation reached; no fabricated success");}
Obj key_color=nullptr,key_retained=nullptr,key_rgb565=nullptr;
}

void rt_graphics_tick(){
    std::vector<Obj> active;for(auto& entry:links)if(entry.second.active&&!entry.second.paused)active.push_back(entry.first);
    double now=media_time();for(Obj o:active){auto& l=links.at(o);if(l.active&&!l.paused&&l.ticks++%l.interval==0){l.timestamp=now;send<void>(l.target,l.selector,o);}}
}
void rt_graphics_run_loop(){
    std::fprintf(stderr,"[UIKit] CADisplayLink loop started\n");
    for(;;){auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(16667);rt_idle_tick();rt_services_tick();rt_drain_main_queue();rt_audio_tick();bool active=false;for(auto& e:links)active|=e.second.active;if(!active)rt_fail("native boot produced no active CADisplayLink");rt_graphics_tick();std::this_thread::sleep_until(deadline);}
}
void rt_install_graphics(){
#define METHOD(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f))
#define CLASS(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f),true)
    METHOD("EAGLContext","initWithAPI:",init_context_simple);METHOD("EAGLContext","initWithAPI:sharegroup:",init_context);
    CLASS("EAGLContext","setCurrentContext:",set_context);CLASS("EAGLContext","currentContext",get_context);
    METHOD("EAGLContext","sharegroup",sharegroup);METHOD("EAGLContext","renderbufferStorage:fromDrawable:",renderbuffer_storage);METHOD("EAGLContext","presentRenderbuffer:",present);
    CLASS("UIScreen","mainScreen",main_screen);METHOD("UIScreen","bounds",bounds);METHOD("UIScreen","applicationFrame",bounds);METHOD("UIScreen","scale",retina_scale);METHOD("UIScreen","nativeScale",retina_scale);
    METHOD("UIScreen","preferredMode",screen_mode);METHOD("UIScreen","currentMode",screen_mode);METHOD("UIScreenMode","size",mode_size);
    for(const char* c:{"UIView","UIWindow","CALayer","CAEAGLLayer"}){
        METHOD(c,"initWithFrame:",init_frame);METHOD(c,"frame",frame);METHOD(c,"bounds",bounds);METHOD(c,"setFrame:",set_frame);METHOD(c,"setBounds:",set_frame);
        METHOD(c,"setOpaque:",bool_arg);METHOD(c,"setHidden:",bool_arg);METHOD(c,"setBackgroundColor:",obj_arg);METHOD(c,"setAutoresizingMask:",integer_arg);
        METHOD(c,"setUserInteractionEnabled:",bool_arg);METHOD(c,"setMultipleTouchEnabled:",bool_arg);METHOD(c,"setClipsToBounds:",bool_arg);
        METHOD(c,"contentScaleFactor",scale);METHOD(c,"setContentScaleFactor:",set_scale);METHOD(c,"contentsScale",scale);METHOD(c,"setContentsScale:",set_scale);
        METHOD(c,"setNeedsLayout",noarg);METHOD(c,"layoutSubviews",noarg);METHOD(c,"layoutIfNeeded",layout_tree);METHOD(c,"layer",layer);CLASS(c,"layerClass",layer_class);
        METHOD(c,"addSubview:",add_subview);METHOD(c,"superview",superview);METHOD(c,"setDelegate:",set_delegate);METHOD(c,"delegate",get_delegate);
        METHOD(c,"becomeFirstResponder",unavailable_keyboard_focus);METHOD(c,"isFirstResponder",no_first_responder);METHOD(c,"canBecomeFirstResponder",no_first_responder);METHOD(c,"resignFirstResponder",resign_unused_responder);
    }
    METHOD("CAEAGLLayer","drawableProperties",props);METHOD("CAEAGLLayer","setDrawableProperties:",set_props);
    for(const char* c:{"UIViewController","UINavigationController"}){
        METHOD(c,"view",get_view);METHOD(c,"setView:",set_view);METHOD(c,"viewDidLoad",noarg);
        METHOD(c,"viewWillAppear:",bool_arg);METHOD(c,"viewDidAppear:",bool_arg);METHOD(c,"viewWillDisappear:",bool_arg);METHOD(c,"viewDidDisappear:",bool_arg);
        METHOD(c,"setWantsFullScreenLayout:",bool_arg);METHOD(c,"setEdgesForExtendedLayout:",integer_arg);METHOD(c,"setDelegate:",set_delegate);METHOD(c,"delegate",get_delegate);
        METHOD(c,"setNavigationBarHidden:animated:",two_obj_arg);METHOD(c,"setNavigationBarHidden:",bool_arg);
    }
    METHOD("UINavigationController","initWithRootViewController:",init_root);METHOD("UINavigationController","topViewController",root);METHOD("UINavigationController","visibleViewController",root);METHOD("UINavigationController","view",nav_view);
    METHOD("UIWindow","setRootViewController:",set_root);METHOD("UIWindow","rootViewController",root);METHOD("UIWindow","makeKeyAndVisible",show_window);
    CLASS("CADisplayLink","displayLinkWithTarget:selector:",new_link);METHOD("CADisplayLink","addToRunLoop:forMode:",add_link);
    METHOD("CADisplayLink","invalidate",invalidate_link);METHOD("CADisplayLink","setPaused:",pause_link);METHOD("CADisplayLink","setFrameInterval:",interval_link);METHOD("CADisplayLink","timestamp",link_time);METHOD("CADisplayLink","duration",link_duration);
    METHOD("UIImage","initWithContentsOfFile:",image_file);CLASS("UIImage","imageNamed:",image_named);METHOD("UIImage","CGImage",ui_cgimage);METHOD("UIImage","size",ui_image_size);METHOD("UIImage","scale",one);
    CLASS("UIColor","blackColor",color_object);CLASS("UIColor","clearColor",color_object);
    key_color=rt_string("EAGLDrawablePropertyColorFormat");key_retained=rt_string("EAGLDrawablePropertyRetainedBacking");key_rgb565=rt_string("RGB565");
}
void* rt_graphics_symbol(const char* s){
#define SYMBOL(n,f) if(std::strcmp(s,n)==0)return reinterpret_cast<void*>(f)
    SYMBOL("_CACurrentMediaTime",media_time);
    SYMBOL("_CGAffineTransformConcat",concat);SYMBOL("_CGAffineTransformInvert",invert);SYMBOL("_CGAffineTransformTranslate",translate);
    SYMBOL("_CGAffineTransformIdentity",const_cast<Affine*>(&affine_identity));SYMBOL("_CGPointZero",const_cast<Point*>(&point_zero));SYMBOL("_CGSizeZero",const_cast<Size*>(&size_zero));SYMBOL("_CGRectZero",const_cast<Rect*>(&rect_zero));
    SYMBOL("_CGRectApplyAffineTransform",rect_transform);SYMBOL("_CGRectContainsPoint",rect_contains);SYMBOL("_CGRectEqualToRect",rect_equal);SYMBOL("_CGRectIntersectsRect",rect_intersects);SYMBOL("_CGRectIsNull",rect_null);
    SYMBOL("_CGPointFromString",point_string);SYMBOL("_CGSizeFromString",size_string);SYMBOL("_CGRectFromString",rect_string);
    SYMBOL("_kEAGLDrawablePropertyColorFormat",&key_color);SYMBOL("_kEAGLDrawablePropertyRetainedBacking",&key_retained);SYMBOL("_kEAGLColorFormatRGB565",&key_rgb565);
    SYMBOL("_CGColorSpaceCreateDeviceRGB",color_rgb);SYMBOL("_CGColorSpaceCreateDeviceGray",color_gray);SYMBOL("_CGColorSpaceRelease",color_release);
    SYMBOL("_CGDataProviderCreateWithData",provider_data);SYMBOL("_CGDataProviderCreateWithCFData",provider_cf);SYMBOL("_CGDataProviderRelease",provider_release);
    SYMBOL("_CGImageCreateWithPNGDataProvider",decode_png);SYMBOL("_CGImageCreate",image_create);SYMBOL("_CGImageGetWidth",image_width);SYMBOL("_CGImageGetHeight",image_height);SYMBOL("_CGImageGetBytesPerRow",image_row);SYMBOL("_CGImageGetBitsPerComponent",image_bpc);SYMBOL("_CGImageGetAlphaInfo",image_alpha);SYMBOL("_CGImageGetColorSpace",image_space);SYMBOL("_CGImageRelease",image_release);
    SYMBOL("_CGBitmapContextCreate",bitmap_create);SYMBOL("_CGBitmapContextCreateImage",bitmap_image);SYMBOL("_CGContextRelease",bitmap_release);SYMBOL("_CGContextScaleCTM",bitmap_scale);SYMBOL("_CGContextTranslateCTM",bitmap_translate);SYMBOL("_CGContextClearRect",bitmap_clear);SYMBOL("_CGContextDrawImage",bitmap_draw);SYMBOL("_UIGraphicsPushContext",push_bitmap);SYMBOL("_UIGraphicsPopContext",pop_bitmap);
    SYMBOL("_glFramebufferRenderbuffer",track_framebuffer_renderbuffer);
    SYMBOL("_glCompileShader",compile_guest_shader);
    SYMBOL("_glShaderSource",guest_shader_source);
    if(std::strcmp(s,"_glRenderbufferStorageMultisampleAPPLE")==0||std::strcmp(s,"_glResolveMultisampleFramebufferAPPLE")==0)return reinterpret_cast<void*>(unsupported_graphics);
    if(std::strncmp(s,"_gl",3)==0){libraries();void* p=dlsym(gles_lib,s+1);if(!p)p=reinterpret_cast<void*>(p_eglGetProcAddress(s+1));return p;}
    for(const char* name:{"_CGContextSetGrayFillColor","_CGContextSetLineWidth","_CGContextSetRGBFillColor","_CGContextSetRGBStrokeColor","_CGContextSetShadow","_CGContextSetTextDrawingMode","_CTFontManagerRegisterFontsForURL","_UIImageJPEGRepresentation","_UIImagePNGRepresentation"})if(std::strcmp(s,name)==0)return reinterpret_cast<void*>(unsupported_graphics);
    return nullptr;
}
