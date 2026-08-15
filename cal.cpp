#include "membrane.hpp"
#include <cstdio>
using namespace sfs;
int main(){
  printf("  calibrating fimp: we want peak head displacement ~0.5 mm at v = 4 m/s\n\n");
  printf("     fimp      peak disp   contact ms  bounces   peak out\n");
  for (float fi : {5e-8f, 1e-7f, 2e-7f, 5e-7f, 1e-6f}) {
    Drum d; d.sr=48000; d.fimp=fi; d.updateStrike(); d.updateModes();
    d.strike(4.f, 0.7f, 0.04f);
    float pd=0, po=0; int inC=0, b=0; bool was=false;
    for(int i=0;i<48000/2;i++){
      bool c = d.mallet.active && (d.mallet.p - d.headDisp) > 0.f;
      float h,s; d.process(h,s);
      if(c){ inC++; if(!was)b++; } was=c;
      if(std::fabs(d.headDisp)>pd) pd=std::fabs(d.headDisp);
      if(std::fabs(h)>po) po=std::fabs(h);
    }
    printf("   %8.1e  %8.3f mm  %8.2f   %6d   %8.3f\n", fi, pd*1000.f, inC*1000.f/48000.f, b, po);
  }
  return 0;
}
