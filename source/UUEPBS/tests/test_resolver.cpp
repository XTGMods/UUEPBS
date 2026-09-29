// Runs the hook resolver against a real game exe on disk (GON, UE 5.7.4).
#include "core/resolver.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using namespace uuepbs;
struct Section { uint32_t va, vsize, raw, rawsize; bool code; };
struct PeView : MemoryView {
  std::vector<uint8_t> file; uint64_t base=0; std::vector<Section> secs;
  explicit PeView(const char* path){ std::ifstream f(path,std::ios::binary); file.assign(std::istreambuf_iterator<char>(f),{});
    uint32_t lfanew; std::memcpy(&lfanew,&file[0x3c],4); uint16_t n; std::memcpy(&n,&file[lfanew+6],2); uint16_t opt; std::memcpy(&opt,&file[lfanew+20],2);
    std::memcpy(&base,&file[lfanew+24+24],8); size_t so=lfanew+24+opt;
    for(int i=0;i<n;i++,so+=40){ Section s; std::memcpy(&s.vsize,&file[so+8],4); std::memcpy(&s.va,&file[so+12],4); std::memcpy(&s.rawsize,&file[so+16],4); std::memcpy(&s.raw,&file[so+20],4);
      uint32_t ch; std::memcpy(&ch,&file[so+36],4); s.code=(ch&0x20000000)!=0; secs.push_back(s);} }
  const Section* at(uint64_t a) const { if(a<base) return nullptr; uint64_t r=a-base; for(auto& s:secs) if(r>=s.va && r<s.va+std::max(s.vsize,s.rawsize)) return &s; return nullptr; }
  bool read(uintptr_t a, void* out, size_t n) const override { const Section* s=at(a); if(!s) return false; uint64_t r=a-base-s->va; if(r+n> s->rawsize) return false; std::memcpy(out,&file[s->raw+r],n); return true; }
  bool is_code(uintptr_t a) const override { const Section* s=at(a); return s && s->code; }
};
int main(int argc,char** argv){
  PeView pe(argc>1?argv[1]:"/mnt/user-data/uploads/Roku3-Win64-Shipping.exe");
  int fails=0;
  for (uint64_t vt_rva : {0x77ED320ull, 0x77EC448ull}) {
    auto c = find_finalize_candidates(pe, pe.base + vt_rva);
    std::printf("vtable rva 0x%llX: %zu candidates\n", (unsigned long long)vt_rva, c.size());
    for (size_t i=0;i<c.size() && i<8;i++) std::printf("  slot %d fn 0x%llX flip 0x%llX editable 0x%X read 0x%X certain %d score %d depth %d\n", c[i].slot,(unsigned long long)(c[i].function-pe.base),(unsigned long long)(c[i].flip-pe.base),c[i].offsets.editable,c[i].offsets.read,c[i].offsets.certain,c[i].score,c[i].depth);
    if (c.empty() || c[0].slot!=374 || c[0].offsets.read!=0x648 || c[0].offsets.editable!=0x644) { std::printf("FAIL: expected slot 374 with 0x644/0x648 first\n"); ++fails; }
    if (!c.empty()) {
      std::printf("  indexed array bases from code:");
      bool has600 = false;
      for (int b = 0; b < c[0].offsets.base_count; ++b) { std::printf(" 0x%X", c[0].offsets.bases[b]); has600 |= c[0].offsets.bases[b] == 0x600; }
      std::printf("\n");
      if (!has600) { std::printf("FAIL: flip code should reveal the pose buffers at 0x600\n"); ++fails; }
    }
    if (c.size()>1 && c[1].score==c[0].score) { std::printf("FAIL: tie for first place\n"); ++fails; }
  }
  // Member-guided search (the UE 5.0 fallback): knowing only the index members, find the slot.
  {
    auto m = find_finalize_by_members(pe, pe.base + 0x77ED320, {0x640, 0x644});
    std::printf("by members: %zu candidates, best slot %d idx +0x%X/+0x%X certain %d score %d\n", m.size(), m.empty() ? -1 : m[0].slot,
                m.empty() ? 0 : m[0].offsets.editable, m.empty() ? 0 : m[0].offsets.read, m.empty() ? 0 : (int)m[0].offsets.certain, m.empty() ? 0 : m[0].score);
    if (m.empty() || m[0].slot != 374 || m[0].offsets.read != 0x648) { std::printf("FAIL: member-guided search should find slot 374\n"); ++fails; }
  }
  // Layout probe against fake components (UE5 double and UE4 float).
  struct Flat : MemoryView { bool read(uintptr_t a, void* o, size_t n) const override { std::memcpy(o,(const void*)a,n); return true; } bool is_code(uintptr_t) const override { return false; } } flat;
  const int N=184;
  std::vector<Xform> d0(N, xf::identity()), d1(N, xf::identity());
  std::vector<XformF> f0(N); for(auto& x:f0){ std::memset(&x,0,sizeof x); x.rot[3]=1; x.scl[0]=x.scl[1]=x.scl[2]=1; }
  alignas(16) static unsigned char comp[0x3000];
  auto put=[&](size_t off, auto v){ std::memcpy(comp+off,&v,sizeof v); };
  auto reset=[&]{ std::memset(comp,0,sizeof comp); for(size_t i=0;i<sizeof comp;i+=8) put(i,(uint64_t)0x7FF0DEADBEEF0000ull+i); };
  reset(); put(0x600, RawArray{d0.data(),N,N}); put(0x610, RawArray{d1.data(),N,N}); put(0x620, RawArray{nullptr,0,0}); put(0x630, RawArray{nullptr,0,0});
  put(0x640,(int32_t)7); put(0x644,(int32_t)0); put(0x648,(int32_t)1);
  FlipOffsets h{0x644,0x648,true};
  auto p1=find_pose_layout(flat,(uintptr_t)comp,N,&h); std::printf("ue5 hint: %s\n",p1.note.c_str());
  if(!p1.layout||p1.layout->buffers!=0x600||p1.layout->read_index!=0x648||p1.layout->elem_size!=96){std::printf("FAIL ue5 hint\n");++fails;}
  auto p2=find_pose_layout(flat,(uintptr_t)comp,N,nullptr); std::printf("ue5 search: %s\n",p2.note.c_str());
  if(!p2.layout||p2.layout->buffers!=0x600||p2.layout->read_index!=0x648){std::printf("FAIL ue5 search\n");++fails;}
  FlipOffsets wrong{0xCB0,0xCB4,false};
  auto p3=find_pose_layout(flat,(uintptr_t)comp,N,&wrong); if(p3.layout){std::printf("FAIL wrong hint accepted\n");++fails;}
  reset(); put(0x5A0, RawArray{f0.data(),N,N}); put(0x5B0, RawArray{f0.data()+0,N,N}); // same data ok? use distinct
  std::vector<XformF> f1=f0; put(0x5B0, RawArray{f1.data(),N,N}); put(0x5C0,(int32_t)1); put(0x5C4,(int32_t)0);
  auto p4=find_pose_layout(flat,(uintptr_t)comp,N,nullptr); std::printf("ue4 search: %s\n",p4.note.c_str());
  if(!p4.layout||p4.layout->elem_size!=48||p4.layout->buffers!=0x5A0||p4.layout->read_index!=0x5C4){std::printf("FAIL ue4 search\n");++fails;}
  reset(); put(0x600, RawArray{nullptr,0,0}); put(0x610, RawArray{nullptr,0,0}); put(0x644,(int32_t)0); put(0x648,(int32_t)0);
  auto p5=find_pose_layout(flat,(uintptr_t)comp,N,&h); std::printf("empty: %s empty=%d\n",p5.note.c_str(),p5.empty_pose);
  if(p5.layout||!p5.empty_pose){std::printf("FAIL empty\n");++fails;}
  // Real layouts: extra bone-sized arrays sit next to the double buffer.
  std::vector<unsigned char> vis(N, 1);
  std::vector<Xform> prev(N, xf::identity());
  for (int read = 0; read <= 1; ++read) {
    // GON 5.7: [0x600,0x610] pose, 0x620 previous visibility (bytes), 0x630 previous pose, ints 0x644/0x648
    reset(); put(0x600, RawArray{d0.data(),N,N}); put(0x610, RawArray{d1.data(),N,N}); put(0x620, RawArray{vis.data(),N,N}); put(0x630, RawArray{prev.data(),N,N});
    put(0x644,(int32_t)(1-read)); put(0x648,(int32_t)read);
    FlipOffsets g{0x644,0x648,true};
    auto a=find_pose_layout(flat,(uintptr_t)comp,N,&g);
    std::printf("gon-like read=%d: %s\n", read, a.note.c_str());
    if(!a.layout||a.layout->buffers!=0x600){std::printf("FAIL gon-like\n");++fails;}
    FlipOffsets gb=g; gb.bases[0]=0x600; gb.bases[1]=0x7B0; gb.base_count=2;
    auto a2=find_pose_layout(flat,(uintptr_t)comp,N,&gb);
    if(!a2.layout||a2.layout->buffers!=0x600){std::printf("FAIL gon-like with code bases\n");++fails;}
    // Wuchang 5.1: [0x610,0x620] pose, 0x630 previous pose, 0x640 previous visibility, ints 0x654/0x658
    reset(); put(0x610, RawArray{d0.data(),N,N}); put(0x620, RawArray{d1.data(),N,N}); put(0x630, RawArray{prev.data(),N,N}); put(0x640, RawArray{vis.data(),N,N});
    put(0x654,(int32_t)(1-read)); put(0x658,(int32_t)read);
    FlipOffsets w{0x654,0x658,false};
    auto b=find_pose_layout(flat,(uintptr_t)comp,N,&w);
    std::printf("wuchang-like read=%d: %s\n", read, b.note.c_str());
    if(!b.layout||b.layout->buffers!=0x610){std::printf("FAIL wuchang-like\n");++fails;}
    auto b2=find_pose_layout(flat,(uintptr_t)comp,N,nullptr);
    if(!b2.layout||b2.layout->buffers!=0x610){std::printf("FAIL wuchang-like memory search\n");++fails;}
  }
  // Stellar Blade (custom UE 4.26): float buffers +0x5A8/+0x5B8, bone visibility +0x7A8, previous pose +0x660,
  // ints +0x670/+0x674 - 0xC8 bytes past the buffers, and the code adds 0x5A8 (not 16-aligned) after the shl.
  for (int read = 0; read <= 1; ++read) {
    std::vector<XformF> g0=f0, g1=f0, gp=f0;
    reset(); put(0x5A8, RawArray{g0.data(),N,N}); put(0x5B8, RawArray{g1.data(),N,N}); put(0x660, RawArray{gp.data(),N,N});
    put(0x650, RawArray{vis.data(),N,N}); put(0x7A8, RawArray{vis.data(),N,N}); put(0x7B8, RawArray{vis.data(),N,N});
    put(0x670,(int32_t)(1-read)); put(0x674,(int32_t)read); put(0x678,(int32_t)12345);
    FlipOffsets sb{0x670,0x674,true}; sb.bases[0]=0x5A8; sb.bases[1]=0x7A8; sb.base_count=2;
    auto a=find_pose_layout(flat,(uintptr_t)comp,N,&sb); std::printf("stellar-blade-like read=%d: %s\n", read, a.note.c_str());
    if(!a.layout||a.layout->buffers!=0x5A8||a.layout->read_index!=0x674||a.layout->elem_size!=48){std::printf("FAIL stellar-blade-like\n");++fails;}
    FlipOffsets nb{0x670,0x674,true};
    auto b=find_pose_layout(flat,(uintptr_t)comp,N,&nb); std::printf("stellar-blade-like, no code bases: %s\n", b.note.c_str());
    if(!b.layout||b.layout->buffers!=0x5A8||b.layout->read_index!=0x674){std::printf("FAIL stellar-blade-like wide window\n");++fails;}
  }
  // Optional: the real Stellar Blade exe, if it is around.
  if (FILE* f = std::fopen("/mnt/user-data/uploads/SB-Win64-Shipping.exe","rb")) {
    std::fclose(f);
    PeView sbx("/mnt/user-data/uploads/SB-Win64-Shipping.exe");
    for (uint64_t vt_rva : {0x5C76DF0ull, 0x63E8880ull}) {
      auto c = find_finalize_candidates(sbx, sbx.base + vt_rva);
      bool base_ok=false; if(!c.empty()) for(int i=0;i<c[0].offsets.base_count;i++) base_ok|=c[0].offsets.bases[i]==0x5A8;
      std::printf("stellar blade vtable 0x%llX: %zu candidates, best slot %d +0x%X/+0x%X bases ok %d\n",(unsigned long long)vt_rva,c.size(),c.empty()?-1:c[0].slot,c.empty()?0:c[0].offsets.editable,c.empty()?0:c[0].offsets.read,(int)base_ok);
      if(c.empty()||c[0].slot!=303||c[0].offsets.editable!=0x670||c[0].offsets.read!=0x674||!base_ok){std::printf("FAIL stellar blade exe\n");++fails;}
    }
  }
  std::printf(fails? "RESOLVER FAILED\n":"resolver OK\n"); return fails; }
