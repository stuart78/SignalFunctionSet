#include "membrane.hpp"
#include <cstdio>
using namespace sfs;
static void probe(float k,float expo,float mass,float v,float&ms,int&b,float&pf){
  Drum d; d.sr=48000; d.updateStrike(); d.updateModes();
  d.mallet.mass=mass; d.mallet.stiff=k; d.mallet.expo=expo;
  d.mallet.strike(v,0.0015f);
  int inC=0; b=0; bool was=false; pf=0;
  for(int i=0;i<48000/4;i++){
    float c=d.mallet.p-d.headDisp; bool con=d.mallet.active&&c>0;
    if(con){ float F=k*std::pow(c,expo); if(F>pf)pf=F; }
    float h,s; d.process(h,s);
    if(con){ inC++; if(!was)b++; } was=con;
  }
  ms=inC*1000.f/48000.f;
}
int main(){
  printf("  contact time vs Hertz constant (mass 0.04, v = 4 m/s)\n");
  printf("  target: ~1 ms for a stick, ~5 ms for a soft mallet\n\n");
  printf("        k        expo   contact ms  bounces  peak force N\n");
  for(float expo : {2.6f, 1.5f}){
    for(float k : {1e5f,1e6f,1e7f,3e7f,1e8f,3e8f}){
      float ms,pf; int b; probe(k,expo,0.04f,4.f,ms,b,pf);
      printf("   %9.1e   %.1f   %9.2f  %6d   %10.1f\n",k,expo,ms,b,pf);
    }
    printf("\n");
  }
  return 0;
}
