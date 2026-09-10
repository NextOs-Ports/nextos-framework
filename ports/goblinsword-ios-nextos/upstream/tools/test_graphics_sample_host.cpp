#include "../prototype/graphics.cpp"
#include <cassert>
#include <fstream>
#include <stdexcept>

[[noreturn]] void rt_fail(const char* reason){throw std::runtime_error(reason);}
namespace fixture {
std::vector<unsigned char> framebuffer;
std::vector<std::pair<int,int>> reads;
GLint pack=8;
GLenum pending_error=GL_NO_ERROR;
int fail_read=0;
bool fail_restore=false;
void get_integer(GLenum key,GLint* value){assert(key==GL_PACK_ALIGNMENT);*value=pack;}
void pixel_store(GLenum key,GLint value){
    assert(key==GL_PACK_ALIGNMENT);pack=value;
    if(value==8&&fail_restore)pending_error=GL_INVALID_VALUE;
}
GLenum get_error(){GLenum result=pending_error;pending_error=GL_NO_ERROR;return result;}
void read_pixels(GLint x,GLint y,GLsizei width,GLsizei height,GLenum format,GLenum type,void* output){
    assert(x==0&&width==screen_w&&y>=0&&y+height<=screen_h);
    assert(format==GL_RGBA&&type==GL_UNSIGNED_BYTE&&pack==1);
    reads.emplace_back(y,height);
    if(int(reads.size())==fail_read){pending_error=GL_INVALID_OPERATION;return;}
    std::memcpy(output,framebuffer.data()+size_t(y)*screen_w*4,size_t(width)*height*4);
}
void clear(){
    framebuffer.assign(size_t(screen_w)*screen_h*4,0);
    // Opaque black is still BLACK; the classifier measures RGB only.
    for(size_t i=3;i<framebuffer.size();i+=4)framebuffer[i]=255;
    reads.clear();fail_read=0;fail_restore=false;pending_error=GL_NO_ERROR;pack=8;
}
std::string receipt_text(const Context& c){
    std::ifstream file(c.proof_directory+"/present-"+std::to_string(c.frame)+".json");assert(file);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
template<class F> void fatal(F operation,const char* reason){
    try{operation();assert(!"unexpected success");}
    catch(const std::runtime_error& error){assert(std::string(error.what()).find(reason)!=std::string::npos);}
}
}
int main(int argc,char** argv){
    assert(argc==2);
    std::string pattern=std::string(argv[1])+"/goblin-sample-host.XXXXXX";
    char* directory=mkdtemp(pattern.data());assert(directory);
    using namespace fixture;
    screen_w=1280;screen_h=720;
    p_glGetIntegerv=get_integer;p_glPixelStorei=pixel_store;p_glGetError=get_error;p_glReadPixels=read_pixels;
    Context c;c.proof_directory=directory;c.frame=1;
    clear();framebuffer[(size_t(7)*screen_w+19)*4]=200;
    FrameSample first=sample_backbuffer(c);
    assert(!std::strcmp(first.mode,"full_initial")&&first.nonblack==1&&first.pixels_tested==921600&&first.pixels_read==921600);
    assert((reads==std::vector<std::pair<int,int>>{{0,720}})&&c.proof_pixels==framebuffer&&pack==8);
    check_black_streak(c,first);assert(c.black==0);

    c.saw_nonblack=true;
    const int row_positions[]={180,360,540};
    std::string text;
    for(int y:row_positions){
        ++c.frame;clear();framebuffer[(size_t(y)*screen_w+19)*4+1]=8;
        FrameSample row=sample_backbuffer(c);
        assert(!std::strcmp(row.mode,"rotating_row")&&row.row_y==y&&row.nonblack==1&&row.pixels_tested==1280&&row.pixels_read==1280);
        assert((reads==std::vector<std::pair<int,int>>{{y,1}})&&pack==8);
        receipt(c,"NONBLACK-PIXELS",row,0);text=receipt_text(c);
        assert(text.find("\"sample_mode\":\"rotating_row\"")!=std::string::npos&&text.find("\"pixels_tested\":1280")!=std::string::npos);
        check_black_streak(c,row);assert(c.black==0);
    }
    assert(c.sample_row_index==0);

    // The current row is dark, but a different row contains the only lit pixel.
    // Every frame must immediately perform a full fallback, not wait until the
    // lit row's turn. Rotation continues while full fallbacks reset BLACK.
    for(int index=0;index<3;++index){
        int y=row_positions[index],lit_y=row_positions[(index+1)%3];
        ++c.frame;clear();framebuffer[(size_t(lit_y)*screen_w+19)*4+2]=4;c.black=59;
        FrameSample fallback=sample_backbuffer(c);
        assert(!std::strcmp(fallback.mode,"full_fallback")&&fallback.row_y==y&&fallback.nonblack==1&&fallback.pixels_tested==921600&&fallback.pixels_read==922880);
        assert((reads==std::vector<std::pair<int,int>>{{y,1},{0,720}})&&pack==8);
        check_black_streak(c,fallback);assert(c.black==0);
    }
    // Also find a point outside all three rotating rows.
    ++c.frame;clear();framebuffer[(size_t(7)*screen_w+19)*4+2]=4;c.black=59;
    FrameSample fallback=sample_backbuffer(c);assert(fallback.nonblack==1&&fallback.pixels_read==922880);
    check_black_streak(c,fallback);assert(c.black==0);
    assert(c.readback_ms>0&&c.scan_ms>0);

    // RGB <= 3 remains below the existing threshold even when alpha is opaque.
    clear();framebuffer[(size_t(180)*screen_w+19)*4]=3;
    for(int frame=1;frame<=59;++frame){
        ++c.frame;reads.clear();FrameSample black=sample_backbuffer(c);
        assert(!std::strcmp(black.mode,"full_fallback")&&black.nonblack==0&&black.pixels_tested==921600&&black.pixels_read==922880);
        check_black_streak(c,black);assert(c.black==unsigned(frame));
    }
    ++c.frame;reads.clear();FrameSample black=sample_backbuffer(c);
    fatal([&]{check_black_streak(c,black);},"60 consecutive black");
    text=receipt_text(c);assert(text.find("\"result\":\"BLACK\"")!=std::string::npos&&text.find("\"pixels_tested\":921600")!=std::string::npos);

    // Any GL error is fatal before the sample can affect the streak, including
    // errors on the first row, full fallback and restoration of PACK_ALIGNMENT.
    for(int scenario=0;scenario<4;++scenario){
        ++c.frame;clear();c.black=9;c.saw_nonblack=scenario!=0;c.sample_row_index=0;
        if(scenario==0||scenario==1)fail_read=1;
        if(scenario==2)fail_read=2;
        if(scenario==3){framebuffer[size_t(180)*screen_w*4]=200;fail_restore=true;}
        fatal([&]{(void)sample_backbuffer(c);},"GL error at physical framebuffer");
        assert(c.black==9&&pack==8);
        text=receipt_text(c);assert(text.find("\"result\":\"DEAD-CONTEXT\"")!=std::string::npos&&text.find("\"gl_error\":0")==std::string::npos);
        assert(reads.size()==size_t(scenario==2?2:1));
    }
    std::printf("PASS: full_initial 921600 pixels; rotating_row 1280 pixels cycles 180/360/540 every frame; full_fallback 921600 tested/922880 read finds other-row and off-row light immediately; 60 full-confirmed BLACK frames; GL errors fatal; pack restored; timing accumulated; receipts=%s\n",directory);
}
