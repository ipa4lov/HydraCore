/**
 \file
 \brief Metropolis Light Transport kernels.

 */

#include "cglobals.h"
#include "crandom.h"
#include "cfetch.h"

#include "clight.h"
#include "cmaterial.h"
#include "cbidir.h"

#define SPLIT_DL_BY_GRAMMAR false

/**
\brief Evaluate contib function. Not used.
\param in_color   - input color
\param out_colors - output contrib value

*/
__kernel void MLTEvalContribFunc(__global const float4* in_color, __global float* out_colors, int iNumElements)
{
  int tid = GLOBAL_ID_X;
  if (tid >= iNumElements)
    return;

  out_colors[tid] = contribFunc(to_float3(in_color[tid]));
}


inline int TabIndex(const int vertId, const int tid, const int iNumElements)
{
  return tid + vertId*iNumElements;
}

__kernel void MMLTInitCameraPath(__global   uint* restrict a_flags,
                                 __global float4* restrict a_color,
                                 __global int2*   restrict a_split,
                                 
                                 //__global RandomGen* restrict a_gens,
                                 //__global float*     restrict a_mmltrands,

                                 const int iNumElements)
{
  const int tid = GLOBAL_ID_X;
  if (tid >= iNumElements)
    return;

  const int d = 3;
  const int s = 0; 

  a_flags[tid] = packBounceNum(0, 1);
  a_color[tid] = make_float4(1,1,1,1);
  a_split[tid] = make_int2(d,s);
}


__kernel void MMLTCameraPathBounce(__global   float4*        restrict a_rpos,
                                   __global   float4*        restrict a_rdir,
                                   __global   uint*          restrict a_flags,
                                   __global RandomGen*       restrict out_gens,
                                  
                                   __global const int2*      restrict in_splitInfo,
                                   __global const Lite_Hit*  restrict in_hits,
                                   __global const int*       restrict in_instLightInstId,
                                   __global const float4*    restrict in_surfaceHit,
                                   __global const float4*    restrict in_procTexData,

                                   __global float4*          restrict a_color,
                                   __global float4*          restrict a_normalPrev,    // (!) stote prev normal here, instead of 'a_thoroughput'
                                   __global MisData*         restrict a_misDataPrev,
                                   __global float4*          restrict a_fog,
                                   __global PdfVertex*       restrict a_pdfVert,       // (!) MMLT pdfArray 
                                   __global   float4*        restrict a_vertexSup,     // (!) MMLT out Path Vertex supplemental to surfaceHit data

                                   __global const float4*    restrict in_texStorage1,    
                                   __global const float4*    restrict in_texStorage2,
                                   __global const float4*    restrict in_mtlStorage,
                                   __global const float4*    restrict in_pdfStorage,   //

                                   __global const EngineGlobals*  restrict a_globals,
                                   const int   iNumElements, 
                                   const float mLightSubPathCount)
{
  const int tid = GLOBAL_ID_X;
  if (tid >= iNumElements)
    return;

  uint flags = a_flags[tid]; // #NOTE: what if ray miss object just recently .. don't we need to do soem thing in MMLT? See original code.
  if (!rayIsActiveU(flags)) 
    return;

  const __global float* a_rptr = 0;   /////////////////////////////////////////////////// #TODO: INIT THIS POINTER WITH MMLT RANDS !!!

  // (0) Read "IntegratorMMLT::CameraPath" arguments and calc ray hit
  //
  const int2 splitData = in_splitInfo[tid];
  const int  d = splitData.x;
  const int  s = splitData.y;
  const int  t = d - s;               // note that s=2 means 1 light bounce and one connection!!!

  const bool a_haveToHitLightSource = (s == 0); // when s == 0, use only camera strategy, so have to hit light at some depth level   
  const int  a_fullPathDepth        = d;       
  const int  a_targetDepth          = t;
  const int  a_currDepth            = unpackBounceNum(flags); // #NOTE: first bounce must be equal to 1                           
  const int  prevVertexId           = a_fullPathDepth - a_currDepth + 1; 

  //__global const PlainLight* pLight = lightAt(a_globals, lightOffset);
  SurfaceHit surfElem;
  ReadSurfaceHit(in_surfaceHit, tid, iNumElements, 
                 &surfElem);

  __global const PlainMaterial* pHitMaterial = materialAt(a_globals, in_mtlStorage, surfElem.matId);

  const float3 ray_pos      = to_float3(a_rpos[tid]);
  const float3 ray_dir      = to_float3(a_rdir[tid]);
  const float3 a_prevNormal = to_float3(a_normalPrev[tid]);

  // (1)
  //
  const float cosHere = fabs(dot(ray_dir, surfElem.normal));
  const float cosPrev = fabs(dot(ray_dir, a_prevNormal));
  
  float GTerm = 1.0f;
  if (a_currDepth == 1)
  {
    float3 camDirDummy; float zDepthDummy;
    const float imageToSurfaceFactor = CameraImageToSurfaceFactor(surfElem.pos, surfElem.normal, a_globals,
                                                                  &camDirDummy, &zDepthDummy);
    const float cameraPdfA = imageToSurfaceFactor / mLightSubPathCount;
   
    PdfVertex vertLast;
    vertLast.pdfRev = cameraPdfA;
    vertLast.pdfFwd = 1.0f;
    a_pdfVert[TabIndex(a_fullPathDepth, tid, iNumElements)] = vertLast; //  a_perThread->pdfArray[a_fullPathDepth]
  }
  else
  {
    const float dist     = length(ray_pos - surfElem.pos); 
    GTerm = cosHere*cosPrev / fmax(dist*dist, DEPSILON2);
  }

  // (2)
  //
  const Lite_Hit liteHit  = in_hits[tid];
  const MisData a_misPrev = a_misDataPrev[tid];

  ProcTextureList ptl;        
  InitProcTextureList(&ptl);  
  ReadProcTextureList(in_procTexData, tid, iNumElements, 
                      &ptl);
  
  const int lightOffset   = (a_globals->lightsNum == 0 || liteHit.instId < 0) ? -1 : in_instLightInstId[liteHit.instId]; // #TODO: refactor this into function!
  __global const PlainLight* pLight = lightAt(a_globals, lightOffset);

  const float3 emission = emissionEval(ray_pos, ray_dir, &surfElem, flags, (a_misPrev.isSpecular == 1), pLight,
                                       pHitMaterial, in_texStorage1, in_pdfStorage, a_globals, &ptl);

  if (dot(emission, emission) > 1e-3f)
  {    
    if (a_currDepth == a_targetDepth && a_haveToHitLightSource)
    {
      const LightPdfFwd lPdfFwd = lightPdfFwd(pLight, ray_dir, cosHere, a_globals, in_texStorage1, in_pdfStorage);
      const float pdfLightWP    = lPdfFwd.pdfW           / fmax(cosHere, DEPSILON);
      const float pdfMatRevWP   = a_misPrev.matSamplePdf / fmax(cosPrev, DEPSILON);
      
      {
        PdfVertex v0,v1;

        v0.pdfFwd = lPdfFwd.pdfA / ((float)a_globals->lightsNum);
        v0.pdfRev = 1.0f;

        v1.pdfFwd = pdfLightWP*GTerm;
        v1.pdfRev = a_misPrev.isSpecular ? -1.0f*GTerm : pdfMatRevWP*GTerm;

        a_pdfVert[TabIndex(0, tid, iNumElements)] = v0;
        a_pdfVert[TabIndex(1, tid, iNumElements)] = v1;
      } 
      
      PathVertex resVertex;
      resVertex.ray_dir  = ray_dir;
      resVertex.accColor = emission*to_float3(a_color[tid]);   
      resVertex.valid    = true;
      WritePathVertexSupplement(&resVertex, tid, iNumElements, 
                                a_vertexSup);

      a_flags[tid] = packRayFlags(flags, unpackRayFlags(flags) | RAY_IS_DEAD);
      return;
    }
    else // this branch could probably change in future, for simple emissive materials
    {
      PathVertex resVertex;
      resVertex.ray_dir  = ray_dir;
      resVertex.accColor = emission*to_float3(a_color[tid]);
      resVertex.valid    = true;
      WritePathVertexSupplement(&resVertex, tid, iNumElements, 
                                a_vertexSup);

      a_flags[tid] = packRayFlags(flags, unpackRayFlags(flags) | RAY_IS_DEAD);
      return;
    } 
  }
  else if (a_currDepth == a_targetDepth && !a_haveToHitLightSource) // #NOTE: what if a_targetDepth == 1 ?
  {
    PathVertex resVertex;
    resVertex.ray_dir      = ray_dir;
    resVertex.valid        = true;
    resVertex.accColor     = make_float3(1, 1, 1)*to_float3(a_color[tid]);
    resVertex.wasSpecOnly  = SPLIT_DL_BY_GRAMMAR ? flagsHaveOnlySpecular(flags) : false;

    if (a_targetDepth != 1)
    {
      const float lastPdfWP = a_misPrev.matSamplePdf / fmax(cosPrev, DEPSILON); // we store them to calculate fwd and rev pdf later when we connect end points
      resVertex.lastGTerm   = GTerm;                                            // because right now we can not do this until we don't know the light vertex
     
      PdfVertex vcurr;
      vcurr.pdfFwd = 1.0f; // write it later, inside ConnectShadow or ConnectEndPoints
      vcurr.pdfRev = a_misPrev.isSpecular ? -1.0f*GTerm : GTerm*lastPdfWP;
      a_pdfVert[TabIndex(prevVertexId, tid, iNumElements)] = vcurr;
    }
    else
      resVertex.lastGTerm = 1.0f;
    
    WritePathVertexSupplement(&resVertex, tid, iNumElements, 
                              a_vertexSup);

    a_flags[tid] = packRayFlags(flags, unpackRayFlags(flags) | RAY_IS_DEAD);
    return;
  }
  
  // (3) sample material, eval reverse and forward pdfs
  //  
  float allRands[MMLT_FLOATS_PER_BOUNCE];
  {
    RandomGen gen  = out_gens[tid];
    gen.maxNumbers = a_globals->varsI[HRT_MLT_MAX_NUMBERS];

    RndMatAll(&gen, a_rptr, a_targetDepth, 0, 0, 0,
              allRands);

    out_gens[tid] = gen;
  }         
  
  MatSample matSam; int localOffset = 0; 
  MaterialSampleAndEvalBxDF(pHitMaterial, allRands, &surfElem, ray_dir, make_float3(1,1,1), flags,
                            a_globals, in_texStorage1, in_texStorage2, &ptl, 
                            &matSam, &localOffset);

  const float3 bxdfVal = matSam.color; // *(1.0f / fmaxf(matSam.pdf, 1e-20f));
  const float cosNext  = fabs(dot(matSam.direction, surfElem.normal));

  if (a_currDepth == 1)
  {
    if (isPureSpecular(matSam))  //  ow ... but if we met specular reflection when tracing from camera, we must put 0 because this path cannot be sample by light strategy at all.
    {                            //  a_perThread->pdfArray[a_fullPathDepth].pdfFwd = 0.0f;
      PdfVertex vertLast = a_pdfVert[TabIndex(a_fullPathDepth, tid, iNumElements)];
      vertLast.pdfFwd    = 0.0f;
      a_pdfVert[TabIndex(a_fullPathDepth, tid, iNumElements)] = vertLast;
    }
  }
  else
  {
    PdfVertex prevVert;

    if (!isPureSpecular(matSam))
    {
      ShadeContext sc;
      sc.wp = surfElem.pos;
      sc.l  = (-1.0f)*ray_dir;  // fliped; if compare to normal PT
      sc.v  = matSam.direction; // fliped; if compare to normal PT
      sc.n  = surfElem.normal;
      sc.fn = surfElem.flatNormal;
      sc.tg = surfElem.tangent;
      sc.bn = surfElem.biTangent;
      sc.tc = surfElem.texCoord;

      const float pdfFwdW  = materialEval(pHitMaterial, &sc, false, false, 
                                          a_globals, in_texStorage1, in_texStorage2, &ptl).pdfFwd;
      const float pdfFwdWP = pdfFwdW / fmax(cosHere, DEPSILON);

      prevVert.pdfFwd = pdfFwdWP*GTerm;
    }
    else
      prevVert.pdfFwd = -1.0f*GTerm;

    const float pdfCamPrevWP = a_misPrev.matSamplePdf / fmax(cosPrev, DEPSILON);
    prevVert.pdfRev = a_misPrev.isSpecular ? -1.0f*GTerm : pdfCamPrevWP*GTerm;
    
    a_pdfVert[TabIndex(prevVertexId, tid, iNumElements)] = prevVert;
  }

  // (4) proceed to next bounce
  //  
  {
    MisData thisBounce       = makeInitialMisData();
    thisBounce.isSpecular    = isPureSpecular(matSam);
    thisBounce.matSamplePdf  = matSam.pdf;
    a_misDataPrev[tid]       = thisBounce;
  }
    
  float3 accColor   = to_float3(a_color[tid]);
  const bool stopDL = SPLIT_DL_BY_GRAMMAR ? flagsHaveOnlySpecular(flags) : false;

  accColor *= (bxdfVal*cosNext / fmax(matSam.pdf, DEPSILON2));
  if (stopDL && a_haveToHitLightSource && a_currDepth + 1 == a_targetDepth) // exclude direct light
    accColor = make_float3(0, 0, 0);

  flags = flagsNextBounce(flags, matSam, a_globals);
  if (maxcomp(accColor) < 0.00001f)
    flags = packRayFlags(flags, unpackRayFlags(flags) | RAY_IS_DEAD);

  const float3 nextRay_dir = matSam.direction;
  const float3 nextRay_pos = OffsRayPos(surfElem.pos, surfElem.normal, matSam.direction);

  a_flags[tid] = flags;
  a_color[tid] = to_float4(accColor,    0.0f);
  a_rpos [tid] = to_float4(nextRay_pos, 0.0f);
  a_rdir [tid] = to_float4(nextRay_dir, 0.0f);
}

