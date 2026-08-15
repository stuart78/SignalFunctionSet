#include "membrane.hpp"
#include <cstdio>
using namespace sfs;
int main(){
  printf("  contact time across EXCITER and velocity (should fall both ways)\n\n");
  printf("   hardness |");
  for(float v:{0.5f,1.f,2.f,4.f,8.f}) printf("  v=%-5.1f", v);
  printf("\n            |");
  for(int i=0;i<5;i++) printf("  --------");
  printf("\n");
  for(float hd:{0.f,0.25f,0.5f,0.75f,1.f}){
    printf("     %.2f    |", hd);
    for(float v:{0.5f,1.f,2.f,4.f,8.f}){
      Drum d; d.sr=48000; d.updateStrike(); d.updateModes();
      d.strike(v,hd,1.f);
      int inC=0,b=0; bool was=false;
      for(int i=0;i<48000/4;i++){
        bool c=d.mallet.active&&(d.mallet.p-d.headDisp)>0.f;
        float hh,ss; d.process(hh,ss);
        if(c){inC++; if(!was)b++;} was=c;
      }
      printf("  %5.2f%s", inC*1000.f/48000.f, b>1?"*":" ");
    }
    printf("\n");
  }
  printf("\n   ms of contact; * = more than one bounce\n");
  return 0;
}
