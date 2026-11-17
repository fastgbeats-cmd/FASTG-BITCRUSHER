// FAST G BITCRUSHER - VICTORY EDITION 3.0 / WARM VINTAGE ENGINE
// SP-1200 architecture-inspired audio model. Not a measured circuit-identical clone.
// Original parameter IDs 0..15, plugin identity and GUI mappings retained.
#include "mpc_plugin.h"
#include <cmath>
#include <cstdint>

namespace {
constexpr float PI = 3.14159265358979323846f;
enum Param { Bits, SampleRate, Drive, Filter, Resonance, Jitter, Mix, Output,
             CrushMode, FilterType, Analog, NoiseFloor, Mono, AutoGain, InputMeter, OutputMeter };
inline float clampf(float x,float a,float b){return x<a?a:(x>b?b:x);}
inline float gain(float d){return std::pow(10.f,d*0.05f);}
inline float soft(float x){return x/(1.f+std::fabs(x));}
inline float randu(uint32_t &s){s^=s<<13;s^=s>>17;s^=s<<5;return float(s&0xffffffu)*(1.f/8388607.5f)-1.f;}
inline float pole(float cutoff,float fs){return 1.f-std::exp(-2.f*PI*clampf(cutoff,1.f,fs*0.45f)/fs);}
struct Voice {
 float dcx=0,dcy=0,aa1=0,aa2=0,held=0,dac=0;
 float ladder[4]={0,0,0,0};
 float fixed[5]={0,0,0,0,0};
 float hpState=0,env=0,prev=0, post=0, inputPeak=0;
 uint32_t rng=0x9e3779b9u;
 void reset(){dcx=dcy=aa1=aa2=held=dac=hpState=env=prev=post=inputPeak=0;
  for(float &v:ladder)v=0;for(float &v:fixed)v=0;}
};
}
class FASTGBitcrusher final: public mpc::Plugin {
public:
 FASTGBitcrusher():mpc::Plugin(info()){
  addParam(mpc::intParam("bits","Bits",2,16,12));
  addParam(mpc::logParam("sample_rate","Sample Rate",1000.f,44100.f,22050.f,"Hz",0));
  addParam(mpc::floatParam("drive","Drive",0.f,24.f,3.f,"dB",1));
  addParam(mpc::logParam("filter","Filter",80.f,18000.f,10000.f,"Hz",0));
  addParam(mpc::floatParam("resonance","Resonance",0.f,90.f,15.f,"%",0));
  addParam(mpc::floatParam("jitter","Jitter",0.f,100.f,0.f,"%",0));
  addParam(mpc::floatParam("mix","Mix",0.f,100.f,75.f,"%",0));
  addParam(mpc::floatParam("output","Output",-24.f,12.f,-3.f,"dB",1));
  addParam(mpc::choiceParam("crush_mode","Crush Mode",{"Clean","Classic","Dirty"},1));
  addParam(mpc::choiceParam("filter_type","Filter Type",{"LP","BP","HP"},0));
  addParam(mpc::boolParam("analog_color","Analog Color",true));
  addParam(mpc::boolParam("noise_floor","Noise Floor",false));
  addParam(mpc::boolParam("mono","Mono",false));
  addParam(mpc::boolParam("auto_gain","Auto Gain",false));
  auto in=mpc::floatParam("input_meter","Input Meter",0,1,0,"",2);in.automatable=false;addParam(in);
  auto out=mpc::floatParam("output_meter","Output Meter",0,1,0,"",2);out.automatable=false;addParam(out);
  addProgram("Clean Crunch",{14,32000,2,15000,8,0,45,-1});
  addProgram("Dub Dust",{10,12000,7,4400,27,12,82,-4});
  addProgram("8 Bit Arcade",{8,8000,3,8000,8,3,100,-5});
  addProgram("Broken Tape",{11,16000,8,3900,22,48,78,-5});
  addProgram("Heavy Crush",{5,5500,13,3100,24,14,100,-9});
  addProgram("Vintage Sampler",{12,22050,5,8500,16,3,100,-3});
  addProgram("Dub Chord Heat",{10,14000,9,5500,32,9,80,-5});
  addProgram("Drum Machine Punch",{12,26000,8,11000,8,0,65,-4});
 }
 void prepare(double sr,int) override {fs_=float(sr>1000?sr:44100);reset();}
 void reset() override {
  for(int c=0;c<2;c++){v_[c].reset();v_[c].rng=0x9e3779b9u+uint32_t(c)*0x6d2b79f5u;}
  phase_=1.f;clock_=1.f;seed_=0x85ebca6bu;
  smMix_=clampf(get(Mix)*.01f,0,1);smDrive_=gain(get(Drive));smOutput_=gain(get(Output));
  smRate_=clampf(get(SampleRate),1000.f,fs_);smCut_=clampf(get(Filter),80.f,fs_*.45f);
 }
 void process(mpc::ProcessContext &ctx) override {
  if(!ctx.outputs||ctx.numFrames<=0||ctx.numOutputs<=0)return;
  const int nc=ctx.numOutputs<2?ctx.numOutputs:2;
  const int mode=getInt(CrushMode), type=getInt(FilterType);
  const bool analog=getBool(Analog),noiseOn=getBool(NoiseFloor),mono=getBool(Mono),autoG=getBool(AutoGain);
  const int bits=int(clampf(float(getInt(Bits)),2,16));
  const float quant=float(1u<<(bits-1));
  const float rateTarget=clampf(get(SampleRate),1000.f,fs_);
  const float driveTarget=gain(get(Drive));
  const float cutTarget=clampf(get(Filter),80.f,fs_*.45f);
  const float outTarget=gain(get(Output));
  const float mixTarget=clampf(get(Mix)*.01f,0,1);
  const float res=clampf(get(Resonance)*.01f,0,.9f);
  const float jitter=clampf(get(Jitter)*.01f,0,1);
  const float smooth=pole(20.f,fs_);
  const float envRelease=std::exp(-1.f/(fs_*.085f));
  // Nominal output is kept conservative for the MPC headphone/speaker path.
  float inPeak=0,outPeak=0;
  for(int i=0;i<ctx.numFrames;i++){
   smMix_+=smooth*(mixTarget-smMix_);smDrive_+=smooth*(driveTarget-smDrive_);
   smOutput_+=smooth*(outTarget-smOutput_);smRate_+=smooth*(rateTarget-smRate_);
   smCut_+=smooth*(cutTarget-smCut_);
   // Classic/Dirty use the vintage 26.04 kHz ceiling. When the host runs
   // below that rate, the clock cannot be represented without oversampling.
   // Classic/Dirty emulate a 26.04 kHz clock when rate knob is at/above it.
   // Below that value, retain creative rate-reduction behaviour.
   const float internalRate=clampf((mode==0)?smRate_:(smRate_<26040.f?smRate_:26040.f),1000.f,fs_);
   const float anti=pole(clampf(internalRate*.42f,150.f,fs_*.43f),fs_);
   const float cutoff=clampf(smCut_,80.f,fs_*.42f);
   const float ladderG=pole(cutoff,fs_);
   // Fixed-channel analogue bandwidth families: 3/4 darker than 5/6.
   // These cutoffs are tonal estimates, NOT extracted calibration values.
   const float fixedG=pole(7900.f,fs_);
   const float highFixedG=pole(11000.f,fs_);
   const bool tick=phase_>=1.f;
   if(tick){phase_-=1.f;clock_=1.f/(1.f+jitter*.5f*randu(seed_));}
   float sum=0;
   if(mono){if(ctx.inputs&&ctx.numInputs>0&&ctx.inputs[0])sum=ctx.inputs[0][i];
    if(ctx.inputs&&ctx.numInputs>1&&ctx.inputs[1])sum=.5f*(sum+ctx.inputs[1][i]);}
   for(int ch=0;ch<nc;ch++){
    if(!ctx.outputs[ch])continue;
    Voice &s=v_[ch];
    const float dry=mono?sum:((ctx.inputs&&ch<ctx.numInputs&&ctx.inputs[ch])?ctx.inputs[ch][i]:0.f);
    inPeak=std::fmax(inPeak,std::fabs(dry));
    float hp=dry-s.dcx+.995f*s.dcy;s.dcx=dry;s.dcy=hp;
    const float driven=hp*smDrive_;
    // Low level is close to linear; additional input nonlinearity in Dirty mode.
    // Subtle level-dependent analogue input: near-linear at normal levels.
    // Avoid broadband clipping and added harmonics at nominal gain.
    const float x=analog?(mode==2?std::tanh(driven*1.15f)/1.15f:std::tanh(driven*.65f)/.65f):clampf(driven,-1.f,1.f);
    s.aa1+=anti*(x-s.aa1);s.aa2+=anti*(s.aa1-s.aa2);
    if(tick){
     // No added dither in vintage modes: preserve quantization spurs.
     const float d=(mode==0?(randu(s.rng)+randu(s.rng))*.25f/quant:0.f);
     const float sample=clampf(s.aa2+d,-1.f,1.f-1.f/quant);
     s.held=std::floor(sample*quant+.5f)/quant;
     if(noiseOn)s.held+=randu(s.rng)*(mode==2?.0012f:.00035f);
     // Transient follower controls a short filter opening, not a fake compressor.
     const float change=std::fabs(s.held-s.prev);s.prev=s.held;
     s.env=std::fmax(s.env,clampf(change*1.25f,0,1));
    }
    s.env*=envRelease;
    // DAC reconstruction is zero-order hold; small bandwidth limitation follows.
    s.dac+=pole(clampf(internalRate*.48f,100.f,fs_*.45f),fs_)*(s.held-s.dac);
    float sig=s.dac;
    // 4-pole nonlinear feedback ladder, inspired by SSM2044 architecture.
    // The exact transistor/OTA behaviour requires calibration to real hardware.
    // Attack opens the dynamic ladder and decay darkens its tail.
    const float modCut=clampf(cutoff*(.55f+1.35f*s.env),80.f,fs_*.42f);
    const float g=pole(modCut,fs_);
    const float feedback=clampf(res*2.8f,0,2.55f);
    float u=sig-feedback*s.ladder[3];
    if(analog)u=std::tanh(u);
    for(int k=0;k<4;k++){
     const float target=analog?std::tanh(u):u;
     s.ladder[k]+=g*(target-s.ladder[k]);
     u=s.ladder[k];
    }
    // Alternative fixed-output coloration (five cascaded poles). It is an
    // approximation, not an exact 1 dB Chebyshev circuit transfer function.
    float f=sig;
    for(int k=0;k<5;k++){
     // First pair approximates lower fixed cutoff; remaining poles preserve
     // the brighter channel-family contribution without adding an effect.
     const float coeff=(k<3?fixedG:highFixedG);
     s.fixed[k]+=coeff*(f-s.fixed[k]);f=s.fixed[k];
    }
    // Filter selector remains LP/BP/HP as required by the existing GUI.
    // LP uses ladder; BP subtracts low band; HP removes ladder low-pass.
    // Blend the two characteristic filtered output families for a single
    // through-effect; real SP-1200 assigns these to separate voices.
    const float low=(mode==0?s.ladder[3]:(.62f*s.ladder[3]+.38f*f));
    s.hpState+=pole(clampf(cutoff*.24f,30.f,fs_*.4f),fs_)*(sig-s.hpState);
    const float band=low-s.hpState;
    const float filtered=type==0?low:(type==1?band:sig-low);
    float wet=filtered;
    if(analog){
     // Calibrated for gentle compression near full scale, not a distortion FX.
     wet=(mode==2?std::tanh(wet*.9f)/.9f:std::tanh(wet*.55f)/.55f);
    }
    if(noiseOn)wet+=randu(s.rng)*.00012f;
    const float mixed=dry+smMix_*(wet-dry);
    const float comp=autoG?1.f/(1.f+.18f*(smDrive_-1.f)):1.f;
    const float y=mixed*smOutput_*comp;
    // Output limiter only for overload; do not alter normal low-level audio.
    const float result=std::fabs(y)<=.95f?y:(y>0?(.95f+.05f*std::tanh((y-.95f)/.05f)):(-.95f+.05f*std::tanh((y+.95f)/.05f)));
    ctx.outputs[ch][i]=std::isfinite(result)?result:0.f;
    outPeak=std::fmax(outPeak,std::fabs(ctx.outputs[ch][i]));
   }
   phase_+=(internalRate/fs_)*clock_;
   phase_=clampf(phase_,0.f,2.f);
  }
  setNormalised(InputMeter,clampf(inPeak,0,1));
  setNormalised(OutputMeter,clampf(outPeak,0,1));
 }
private:
 static mpc::PluginInfo info(){mpc::PluginInfo x;x.name="FAST G BITCRUSHER";
  x.vendor="FAST G STUDIOS";x.uniqueId=MPC_FOURCC('F','G','B','1');
  x.version=3000;x.isInstrument=false;x.numInputs=2;x.numOutputs=2;x.tailSamples=1;return x;}
 Voice v_[2];uint32_t seed_=0x85ebca6bu;float fs_=44100.f,phase_=1.f,clock_=1.f;
 float smMix_=.75f,smDrive_=1.f,smOutput_=1.f,smRate_=22050.f,smCut_=10000.f;
};
MPC_PLUGIN_ENTRY(FASTGBitcrusher)
