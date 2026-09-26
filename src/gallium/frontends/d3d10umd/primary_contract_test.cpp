#include "PrimaryContract.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

constexpr unsigned DXGI_DDI_PRIMARY_OPTIONAL=1, DXGI_DDI_PRIMARY_NONPREROTATED=2;
constexpr unsigned DXGI_DDI_MODE_ROTATION_IDENTITY=1, D3D10DDIRESOURCE_TEXTURE2D=3;
struct Device { void *runtime_present; };
struct DXGI_DDI_PRIMARY_DESC {
   unsigned Flags, VidPnSourceId;
   struct {
      unsigned Width, Height, Rotation, Format;
      struct { unsigned Numerator, Denominator; } RefreshRate;
   } ModeDesc;
};
struct Mip { unsigned TexelWidth, TexelHeight, TexelDepth; };
struct D3D10DDIARG_CREATERESOURCE {
   DXGI_DDI_PRIMARY_DESC *pPrimaryDesc;
   Mip *pMipInfoList;
   unsigned Format, MipLevels, ArraySize;
   struct { unsigned Count, Quality; } SampleDesc;
   unsigned ResourceDimension;
};
bool flipOptional=true;
bool FlipOptionalPrimaries() { return flipOptional; }
// INSERT_PRODUCTION

int main()
{
   Device device{reinterpret_cast<void *>(1)};
   DXGI_DDI_PRIMARY_DESC primary{1,0,{3040,1904,1,87,{165,1}}};
   Mip mip{3040,1904,1};
   D3D10DDIARG_CREATERESOURCE create{&primary,&mip,87,1,1,{1,0},3};
   assert(PrimaryCanScanOut(&device,&create));
   assert(OptionalPrimaryCanScanOut(&device,&create));
   flipOptional=false;
   assert(!OptionalPrimaryCanScanOut(&device,&create) && PrimaryCanScanOut(&device,&create));
   flipOptional=true;
   for(unsigned flags : {0U,1U,2U,3U}) {
      primary.Flags=flags;
      assert(PrimaryCanScanOut(&device,&create));
      for(unsigned rotation : {0U,2U,3U,4U,5U}) {
         primary.ModeDesc.Rotation=rotation;
         assert(!PrimaryCanScanOut(&device,&create));
         assert(!OptionalPrimaryCanScanOut(&device,&create));
      }
      primary.ModeDesc.Rotation=1;
   }
   primary.Flags=1;
   for(unsigned fault=0;fault!=13;++fault) {
      auto p=primary; auto m=mip; auto c=create; auto d=device;
      c.pPrimaryDesc=&p; c.pMipInfoList=&m;
      switch(fault) {
      case 0: p.Flags=4; break;
      case 1: p.VidPnSourceId=1; break;
      case 2: p.ModeDesc.Format=28; break;
      case 3: p.ModeDesc.RefreshRate.Numerator=0; break;
      case 4: p.ModeDesc.RefreshRate.Denominator=0; break;
      case 5: m.TexelWidth=1904; m.TexelHeight=3040; break;
      case 6: m.TexelDepth=2; break;
      case 7: c.MipLevels=2; break;
      case 8: c.ArraySize=2; break;
      case 9: c.SampleDesc.Count=4; break;
      case 10: c.SampleDesc.Quality=1; break;
      case 11: c.ResourceDimension=1; break;
      case 12: d.runtime_present=nullptr; break;
      }
      assert(!PrimaryCanScanOut(&d,&c));
   }
   auto absent=create; absent.pPrimaryDesc=nullptr;
   assert(!PrimaryCanScanOut(&device,&absent));
   absent=create; absent.pMipInfoList=nullptr;
   assert(!PrimaryCanScanOut(&device,&absent));

   // Synthetic matched-producer contract tests only. The live call above has
   // no profile and never enables this branch. Mode representation is explicit.
   DroidvmPrimaryProfile profile{{1,64,1904,3040,3040,1904,1,0,7,11,{0,0}},
                                  {true,true,true,true,true,true},3040,1904,4};
   DroidvmPrimaryDescription p{3,0,3040,1904,4,87,165,1,1904,3040,1,87,1,1,1,0,true,true};
   assert(DroidvmPrimaryCanScanOut(p,&profile));
   assert(!DroidvmPrimaryCanScanOut(p));
   auto wrong=p; wrong.flags=1;
   assert(!DroidvmPrimaryCanScanOut(wrong,&profile)); // Generic rotated resource needs transforms.
   wrong=p; wrong.width=3040; wrong.height=1904;
   assert(!DroidvmPrimaryCanScanOut(wrong,&profile)); // Logical texture cannot become portrait storage.
   wrong=p; wrong.mode_rotation=2;
   assert(!DroidvmPrimaryCanScanOut(wrong,&profile)); // Do not infer DXGI sign from VidPN.
   for(unsigned missing=0;missing!=6;++missing) {
      auto bad=profile;
      switch(missing) {
      case 0: bad.readiness.HostGeometry=false; break;
      case 1: bad.readiness.ProducerFinalRender=false; break;
      case 2: bad.readiness.RotationAwarePrimary=false; break;
      case 3: bad.readiness.ModeCommitAndUpdate=false; break;
      case 4: bad.readiness.ResourceBinding=false; break;
      case 5: bad.readiness.AllPresentPaths=false; break;
      }
      assert(!DroidvmPrimaryCanScanOut(p,&bad));
   }
   auto diagnostic=profile;
   diagnostic.readiness={}; diagnostic.diagnostic=true; diagnostic.diagnostic_mechanisms=63;
   assert(DroidvmPrimaryCanScanOut(p,&diagnostic) && !VioGpuScanoutReady(diagnostic.readiness));
   for(unsigned bit=0;bit<6;++bit) {
      auto bad=diagnostic; bad.diagnostic_mechanisms &= ~(1U<<bit);
      assert(!DroidvmPrimaryCanScanOut(p,&bad));
   }
   diagnostic.diagnostic_mechanisms=127;
   assert(!DroidvmPrimaryCanScanOut(p,&diagnostic));
   puts("PASS actual primary admission: identity, flags, mip extent/shape, all nonidentity gated; synthetic matched-profile prerequisites");
}
