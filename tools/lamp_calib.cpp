// 阈值校准台。
//
//   c++ -std=c++17 -O2 -I usermods/lamp -I tools tools/lamp_calib.cpp -o /tmp/calib && /tmp/calib
//
// 六段配器完整的合成素材过完整管线，打印出**自动选灯效实际消费的那几个统计量**：
// 能量加权的打击占比、分层度、f0 有声占比与抖动率、节拍置信度。
//
// 存在的理由：`lamp_auto.h` 里那几个门槛必须落在实测分布的**空隙**里，
// 而不是某一簇的内部（docs/17 第 26 条）。没有这张表就只能拍脑袋。
//
// ⚠️ **量的必须是代码实际用的那个统计量。** 我照着逐帧瞬时值改过一次
// `perc_hi`（0.55 → 0.72），结果 Rap 与电子双双掉回兜底 —— 它们的瞬时值是
// 0.93/1.00，但**能量加权平均**只有 0.67/0.66。原值本来就是对的。
//
// 改动判据或加了新流派素材之后，重跑这个再决定阈值。
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "lamp_auto.h"
#include "lamp_fft.h"
using namespace lamp;
static float pcm[2048], re[2048], im[2048], mag[1025], win[2048];
static uint32_t sd=1;
// **必须与 tools/ 下那个 Python 探针逐位一致**，否则「同一段素材」其实不是同一段。
// 我为此对不上过一次：C++ 版多了一次 n^=n>>13，噪声序列不同，
// 于是本机校准与线上探针在摇滚这一档给出了不同的结论，白查了一轮。
static float rnd(int i){
  uint32_t n=(uint32_t)((int64_t)i*1664525+1013904223) & 0xFFFFFFFFu;
  n^=n>>16; n=(uint32_t)((uint64_t)n*2246822519u & 0xFFFFFFFFu);
  return ((n&0xFFFF)/32768.0f)-1.0f; }
static const float BPM=120.0f, per=60.0f/BPM;

static float classical(float t){
  const float sw=0.25f+0.75f*(0.5f-0.5f*cosf(6.2831853f*t/18.0f));
  static const int sc[5]={0,3,5,7,10}; const float f=196.0f*powf(2.0f,sc[(int)(t/1.7f)%5]/12.0f);
  float v=0; for(int h=1;h<=5;++h) v+=sinf(6.2831853f*f*h*t)/h; return sw*0.5f*v; }
static float pop(float t,int i){
  const float ph=fmodf(t,per)/per; static const int sc[8]={0,2,4,5,7,5,4,2};
  const float f=329.63f*powf(2.0f,sc[(int)(t/0.5f)%8]/12.0f);
  float v=0; for(int h=1;h<=5;++h) v+=0.7f*sinf(6.2831853f*f*h*t)/h;
  v+=0.3f*expf(-ph*13)*rnd(i); return v*0.5f; }
static float rockCore(float t,int i,bool voc){
  const float bar=per*4; const int idx=(int)(t/bar);
  static const float roots[4]={82.41f,98.00f,110.00f,82.41f};
  float v=0;
  for(int k=0;k<2;++k){ int b=idx-k; if(b<0) continue;
    const float env=k?expf(-(t-b*bar)*1.2f):1.0f; if(env<0.02f) continue;
    const float root=roots[b%4];
    for(int d=0;d<2;++d){ const float f=root*(d?1.004f:0.996f);
      const float ml[3]={1.0f,1.5f,2.0f}, am[3]={1.0f,0.9f,0.7f};
      for(int m=0;m<3;++m) v+=env*am[m]*sinf(6.2831853f*f*ml[m]*t); } }
  v=tanhf(2.0f*v);
  if(voc){ static const int sc[6]={0,3,5,7,5,3}; const float fv=246.94f*powf(2.0f,sc[(int)(t/0.75f)%6]/12.0f);
    for(int h=1;h<=4;++h) v+=0.55f*sinf(6.2831853f*fv*h*t)/h; }
  const float ph=fmodf(t,per)/per; const int beat=(int)(t/per)%4; const float e=expf(-ph*11);
  if(beat==0||beat==2) v+=0.9f*e*(0.7f*sinf(6.2831853f*(55+30*expf(-ph*25))*t)+0.5f*rnd(i));
  else                 v+=0.85f*e*rnd(i);
  const float hh=fmodf(t,per/2)/(per/2);
  v+=0.28f*expf(-hh*40)*rnd(i*3);
  v+=0.30f*rnd(i*5)*(0.6f+0.4f*sinf(6.2831853f*t*3.1f));
  return v*0.35f; }
static float rap(float t,int i){
  const float ph=fmodf(t,per)/per, hh=fmodf(t,per/4)/(per/4);
  float v=0.9f*expf(-ph*6)*sinf(6.2831853f*48*t);
  v+=0.35f*expf(-hh*60)*rnd(i*7)*(hh<0.2f?1.0f:0.0f); return v*0.6f; }
static float edm(float t,int i){
  const float ph=fmodf(t,per)/per;
  float v=0.9f*expf(-ph*7)*sinf(6.2831853f*(55+25*expf(-ph*22))*t);
  const float cut=600+3000*(0.5f-0.5f*cosf(6.2831853f*t/12.0f));
  v+=0.35f*sinf(6.2831853f*cut*t)*(0.5f+0.5f*sinf(6.2831853f*t/12.0f)); return v*0.55f; }

static float gen(int which,float t,int i){
  switch(which){case 0:return classical(t);case 1:return pop(t,i);
  case 2:return rockCore(t,i,false);case 3:return rockCore(t,i,true);
  case 4:return rap(t,i);case 5:return edm(t,i);
  // 摇滚，人声每 8 秒开关一次 —— **这才是真实情形**。
  // 「人声全程都在」的素材（case 3）里没有无人声的参照段，
  // 相对通路本来就该报 0；能不能在交替时抓到，才是它的判据。
  default:return rockCore(t,i,fmodf(t,16.0f)<8.0f);} }

int main(int argc,char**argv){
  const float RUN_MS = (argc>1)?(float)atof(argv[1]):45000.0f;
  const char* NM[7]={"古典·柔板","流行","摇滚·器乐","摇滚·人声","Rap","电子","摇滚·断续人声"};
  double VS[7]={0},VF[7]={0},VC[7]={0},VH[7]={0},VP[7]={0},VD[7]={0};
  double VA[7]={0};
  const char* FX[10]={"频段柱","拍点脉冲","电平扫描","冲击柱","拍点光点","高低分离","调性染色","音级环","彩色流动","旋律线"};
  printf("%-12s %7s %6s %6s %7s %5s %5s  %s %6s | %s\n","","perc","split","f0占","抖动/s","锁定","调性","档","时长","自动选中(五个分数)");
  for(int w=0;w<7;++w){
    static Pipeline P; static PipelineConfig PC; static AutoState A; static AutoConfig AC;
    P=Pipeline{}; A=AutoState{};
    pipelineInit(P,PC,STYLE_GENERAL); autoInit(A,AC,presetHopMs(STYLE_GENERAL));
    size_t consumed=0; float lock=0, key=0; int ps=1; uint32_t tend=0;
    double vsum=0,fsum=0,csum=0,hsum=0,psum=0,asum=0; long vn=0,vhi=0;
    const int NFR=(int)(RUN_MS/presetHopMs(STYLE_GENERAL));
    for(int frame=0;frame<NFR;++frame){
      const size_t n=P.an.n;
      for(size_t k=0;k<n;++k){ const int idx=(int)(consumed+k); pcm[k]=gen(w,(float)idx/kSampleRate,idx); }
      fillWindow(P.an.wt,win,n); magnitudeSpectrum(pcm,win,n,re,im,mag);
      const uint32_t t_ms=(uint32_t)((double)consumed*1000.0/kSampleRate);
      const AudioFrame f=pipelineProcess(P,PC,pcm,mag,t_ms);
      autoUpdate(A,AC,f,t_ms); lock=f.bpm_conf; key=f.key_conf; ps=(int)f.preset; tend=t_ms;
      if(!f.gated){ ++vn; vsum+=f.vocal; fsum+=formantShare(PC.vocal,f.bands_h);
        csum+=(f.f0_voiced?f.f0_conf:0.0f); hsum+=1.0f-f.percussive;
        asum+=P.vocal.raw;
        if(f.vocal>0.55f) ++psum; if(f.f0_voiced) ++vhi; }
      consumed+=(size_t)(presetHopMs(P.style.current)*kSampleRate/1000.0f);
    }
    const float pa=(A.e_h+A.e_p>1e-12f)?A.e_p/(A.e_h+A.e_p):0.0f;
    printf("%-12s %7.2f %6.2f %6.2f %7.1f %5.2f %5.2f  档%d %5.1fs | %-8s (%.2f/%.2f/%.2f/%.2f/%.2f)\n",
           NM[w], pa, splitContrastOf(A.e_ends,A.e_mid), A.f0_duty, A.f0_jitter, lock, key,
           ps, tend/1000.0f, FX[A.current],
           A.score[0],A.score[1],A.score[2],A.score[3],A.score[4]);
    VS[w]=vsum/(double)vn; VF[w]=fsum/(double)vn; VC[w]=csum/(double)vn;
    VH[w]=hsum/(double)vn; VP[w]=psum/(double)vn; VD[w]=(double)vhi/(double)vn;
    VA[w]=asum/(double)vn;
  }

  // ── 人声存在度的标定表 ────────────────────────────────────
  //
  // 「摇滚·器乐」与「摇滚·人声」是同一段伴奏加不加人声线，天然的正反例对。
  // 门槛要落在这两列之间的**空隙**里，不能落在任一簇内部（第 26 条）。
  // 三个因子分开打印，才知道是哪一项在拖后腿（第 33 条：量代码实际用的量）。
  printf("\n%-14s %7s %8s %8s | %7s %8s | %7s\n",
         "人声标定","f0conf","共振峰","1-打击","raw","vocal","过0.55占");
  for(int w=0;w<7;++w)
    printf("%-14s %7.2f %8.2f %8.2f | %7.2f %8.2f | %7.2f\n",
           NM[w], VC[w], VF[w], VH[w], VA[w], VS[w], VP[w]);
  return 0; }
