#include "core/mirror.hpp"
#include "core/rig_names.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cassert>
using namespace uuepbs;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++fails;} }while(0)
static bool point_mirrored(std::string base){ // old hardcoded Roku table
  static const char* limb[]={"ik_foot","ik_hand","clavicle","upperarm","lowerarm","hand","index_metacarpal","index_01","index_02","index_03","middle_metacarpal","middle_01","middle_02","middle_03","pinky_metacarpal","pinky_01","pinky_02","pinky_03","ring_metacarpal","ring_01","ring_02","ring_03","thumb_01","thumb_02","thumb_03","lowerarm_twist_01","upperarm_twist_01","thigh","calf","calf_twist_01","foot","ball","thigh_twist_01"};
  base=fold_case(base); if(base.ends_with("_l")||base.ends_with("_r")) base.resize(base.size()-2);
  for(auto l:limb) if(base==l) return true; return false;}
int main(){
  CHECK(mirror_bone_name("thigh_l")=="thigh_r");
  CHECK(mirror_bone_name("horn1_l_001")=="horn1_r_001");
  CHECK(mirror_bone_name("mixamorig:LeftUpLeg")=="mixamorig:RightUpLeg");
  CHECK(mirror_bone_name("Thigh.L")=="Thigh.R");
  CHECK(mirror_bone_name("DEF-thigh.L")=="DEF-thigh.R");
  CHECK(mirror_bone_name("lThighBend")=="rThighBend");
  CHECK(mirror_bone_name("Bip001 L Thigh")=="Bip001 R Thigh");
  CHECK(mirror_bone_name("J_Bip_L_UpperArm")=="J_Bip_R_UpperArm");
  CHECK(mirror_bone_name("CC_Base_L_Thigh")=="CC_Base_R_Thigh");
  CHECK(mirror_bone_name("hand_left")=="hand_right");
  CHECK(mirror_bone_name("RIGHT_ARM")=="LEFT_ARM");
  CHECK(mirror_bone_name("L_Hand")=="R_Hand");
  CHECK(mirror_bone_name("lowerarm").empty());
  CHECK(mirror_bone_name("root").empty());
  CHECK(mirror_bone_name("Leftover").empty());
  CHECK(mirror_bone_name("spine_01").empty());
  CHECK(mirror_bone_name("pelvis").empty());
  CHECK(mirror_bone_name("ball_l")=="ball_r");
  CHECK(bone_stem("mixamorig:LeftUpLeg")=="upleg");
  CHECK(bone_stem("J_Bip_L_UpperArm")=="upperarm");
  CHECK(bone_stem("thigh_twist_01_l")=="thightwist01");
  CHECK(bone_stem("CC_Base_L_Thigh")=="thigh");
  CHECK(bone_stem("Bip001 L Thigh")=="thigh");
  CHECK(bone_stem("lThighBend")=="thighbend");
  CHECK(bone_stem("DEF-thigh.L")=="thigh");
  CHECK(bone_stem("J_Bip_C_Spine")=="spine");
  // Roku ref pose
  std::vector<std::string> names; std::vector<int32_t> parents; std::vector<Xform> pose;
  std::ifstream f("roku_ref.txt"); std::string line;
  while(std::getline(f,line)){ std::istringstream in(line); std::string n; int p; Xform x=xf::identity();
    in>>n>>p>>x.rot[0]>>x.rot[1]>>x.rot[2]>>x.rot[3]>>x.pos[0]>>x.pos[1]>>x.pos[2]>>x.scl[0]>>x.scl[1]>>x.scl[2];
    names.push_back(n); parents.push_back(p); pose.push_back(x);}
  MirrorTable t=build_mirror_table(names,parents,pose,true);
  std::printf("centre axis %d, pairs %zu, inexact %zu\n",t.centre_axis,t.pairs,t.inexact);
  CHECK(t.centre_axis==0);
  int checked=0;
  for(auto& n:names){ auto it=t.rules.find(fold_case(n)); if(it==t.rules.end()) continue; ++checked;
    const MirrorRule& r=it->second; bool pm=point_mirrored(n);
    bool ident = r.source[0]==0&&r.source[1]==1&&r.source[2]==2;
    bool ok = ident && (pm ? (r.move_sign[0]==-1&&r.move_sign[1]==-1&&r.move_sign[2]==-1&&r.turn_sign[0]==1&&r.turn_sign[1]==1&&r.turn_sign[2]==1)
                        : (r.move_sign[0]==-1&&r.move_sign[1]==1&&r.move_sign[2]==1&&r.turn_sign[0]==1&&r.turn_sign[1]==-1&&r.turn_sign[2]==-1));
    if(!ok){ std::printf("  mismatch %s src %d%d%d move %d%d%d turn %d%d%d exact %d\n",n.c_str(),r.source[0],r.source[1],r.source[2],r.move_sign[0],r.move_sign[1],r.move_sign[2],r.turn_sign[0],r.turn_sign[1],r.turn_sign[2],r.exact); ++fails;}
  }
  std::printf("checked %d sided bones\n",checked);
  // live-pose tolerance: perturb local rotations by up to ~20 deg randomly and re-measure
  unsigned s=12345; auto rnd=[&]{ s=s*1103515245+12345; return ((s>>8)&0xFFFF)/65535.0*2-1; };
  std::vector<Xform> noisy=pose; for(auto& x:noisy){ double h=0.05; double d[4]={rnd()*h,rnd()*h,rnd()*h,1}; double o[4]; xf::qmul(x.rot,d,o); for(int i=0;i<4;i++) x.rot[i]=o[i]; }
  MirrorTable t2=build_mirror_table(names,parents,noisy,true); int diff=0;
  for(auto& [k,r]:t.rules){ auto& r2=t2.rules[k]; for(int i=0;i<3;i++) if(r.move_sign[i]!=r2.move_sign[i]||r.source[i]!=r2.source[i]) {++diff;break;} }
  std::printf("noisy pose: %d of %zu rules differ\n",diff,t.rules.size());
  std::printf(fails?"FAILED %d\n":"all mirror tests passed\n",fails); return fails?1:0; }
