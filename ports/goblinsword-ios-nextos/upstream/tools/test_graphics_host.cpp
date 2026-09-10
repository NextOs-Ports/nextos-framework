#include "../prototype/graphics.cpp"
#include <cassert>
#include <stdexcept>
#include <fstream>
[[noreturn]] void rt_fail(const char* s){throw std::runtime_error(s);}
int main(int argc,char** argv){
 Rect a{{10,20},{30,40}};
 assert(rect_contains(a,{10,20})); assert(!rect_contains(a,{40,25}));
 assert(!rect_contains(a,{20,60})); assert(rect_contains({{40,60},{-30,-40}},{20,30}));
 assert(!rect_intersects(a,{{40,20},{30,40}})); assert(rect_intersects(a,{{39,20},{30,40}}));
 Affine t{2,0,0,3,5,7};Point p{4,6}; auto back=transform_point(transform_point(p,t),invert(t));
 assert(std::abs(back.x-p.x)<1e-10 && std::abs(back.y-p.y)<1e-10);
 auto r=rect_transform({{0,0},{4,5}},{0,1,-1,0,0,0});assert(rect_equal(r,{{-5,0},{5,4}}));
 Image source;source.width=2;source.height=1;source.rgba={200,100,50,128, 20,40,60,255};
 unsigned char raw[8]{};auto* bitmap=bitmap_create(raw,2,1,8,8,&rgb_space,1);bitmap_draw(bitmap,{{0,0},{2,1}},&source);
 assert(raw[0]==100&&raw[1]==50&&raw[2]==25&&raw[3]==128&&raw[4]==20&&raw[7]==255);bitmap_release(bitmap);
 std::memset(raw,0,sizeof(raw));bitmap=bitmap_create(raw,2,1,8,8,&rgb_space,0x4001);assert(bitmap->info==0x4001);bitmap_draw(bitmap,{{0,0},{2,1}},&source);
 assert(raw[0]==100&&raw[1]==50&&raw[2]==25&&raw[3]==128&&raw[4]==20&&raw[7]==255);bitmap_release(bitmap);
 std::memset(raw,0,sizeof(raw));bitmap=bitmap_create(raw,2,1,8,8,&rgb_space,0x4005);assert(bitmap->info==0x4005);bitmap_draw(bitmap,{{0,0},{2,1}},&source);
 assert(raw[0]==200&&raw[1]==100&&raw[2]==50&&raw[3]==255&&raw[4]==20&&raw[7]==255);bitmap_release(bitmap);
 size_t count=0;
 for(int n=1;n<argc;n++){std::ifstream file(argv[n],std::ios::binary);Provider p;p.bytes.assign(std::istreambuf_iterator<char>(file),{});auto* im=decode_png(&p,nullptr,false,0);assert(im->width && im->height && im->rgba.size()==im->width*im->height*4);++count;image_release(im);}
 std::printf("PASS geometry, half-open edges, affine inverse, bitmap premultiplication; decoded %zu original PNG files\n",count);
}
