#ifndef RTCBIDIR
#define RTCBIDIR

#include "cglobals.h"
#include "cfetch.h"
#include "cmaterial.h"
#include "clight.h"


typedef struct PathVertexT
{
  SurfaceHit hit;
  float3     ray_dir;
  float3     accColor;
  float      lastGTerm;
  bool       valid;
  bool       wasSpecOnly; ///< Was Specular Only. Exclude Direct Light - ES*(D|G)L or ES*L.
} PathVertex;

typedef struct PdfVertexT
{
  float pdfFwd;
  float pdfRev;
} PdfVertex;

static inline void InitPathVertex(__private PathVertex* a_pVertex) 
{
  a_pVertex->valid       = false; 
  a_pVertex->lastGTerm   = 1.0f;
  a_pVertex->accColor    = make_float3(1, 1, 1);
  a_pVertex->wasSpecOnly = false;
}

#define PV_PACK_VALID_FIELD 1
#define PV_PACK_WASSP_FIELD 2
#define PV_PACK_HITFI_FIELD 4 // Hit From Inside 

#define PATH_VERTEX_SIZE_IN_F4 5

static inline void WritePathVertex(const __private PathVertex* a_pVertex, int a_tid, int a_threadNum, 
                                   __global float4* a_out)
{
  float4 f1 = to_float4(a_pVertex->hit.pos,    a_pVertex->hit.texCoord.x);
  float4 f2 = to_float4(a_pVertex->hit.normal, a_pVertex->hit.texCoord.y);

  float4 f3 = make_float4(encodeNormal(a_pVertex->hit.flatNormal), 
                          encodeNormal(a_pVertex->hit.tangent), 
                          encodeNormal(a_pVertex->hit.biTangent), 
                          as_float(a_pVertex->hit.matId));

  // ignore (hit.t, hit.sRayOff) because bpt don't need them! 

  const int bit1 = a_pVertex->valid       ? PV_PACK_VALID_FIELD : 0;
  const int bit2 = a_pVertex->wasSpecOnly ? PV_PACK_WASSP_FIELD : 0;
  const int bit3 = a_pVertex->hit.hfi     ? PV_PACK_HITFI_FIELD : 0;

  float4 f4 = to_float4(a_pVertex->ray_dir, a_pVertex->lastGTerm);
  float4 f5 = to_float4(a_pVertex->accColor, as_float(bit1 | bit2 | bit3));

  a_out[a_tid + 0*a_threadNum] = f1;
  a_out[a_tid + 1*a_threadNum] = f2;
  a_out[a_tid + 2*a_threadNum] = f3;
  a_out[a_tid + 3*a_threadNum] = f4;
  a_out[a_tid + 4*a_threadNum] = f5;
} 

static inline void ReadPathVertex(const __global float4* a_in, int a_tid, int a_threadNum, 
                                  __private PathVertex* a_pVertex)
{
  const float4 f1 = a_in[a_tid + 0*a_threadNum];
  const float4 f2 = a_in[a_tid + 1*a_threadNum];
  const float4 f3 = a_in[a_tid + 2*a_threadNum];
  const float4 f4 = a_in[a_tid + 3*a_threadNum];
  const float4 f5 = a_in[a_tid + 4*a_threadNum];

  a_pVertex->hit.pos        = to_float3   (f1); a_pVertex->hit.texCoord.x = f1.w;
  a_pVertex->hit.normal     = to_float3   (f2); a_pVertex->hit.texCoord.y = f2.w;
  a_pVertex->hit.flatNormal = decodeNormal(f3.x);
  a_pVertex->hit.tangent    = decodeNormal(f3.y);
  a_pVertex->hit.biTangent  = decodeNormal(f3.z);
  a_pVertex->hit.matId      = as_int      (f3.w);

  a_pVertex->ray_dir  = to_float3(f4); a_pVertex->lastGTerm = f4.w;
  a_pVertex->accColor = to_float3(f5); 

  const int flags        = as_int(f5.w);

  a_pVertex->valid       = ((flags & PV_PACK_VALID_FIELD) != 0);
  a_pVertex->wasSpecOnly = ((flags & PV_PACK_WASSP_FIELD) != 0);
  a_pVertex->hit.hfi     = ((flags & PV_PACK_HITFI_FIELD) != 0);
} 



/**
\brief  Compute pdf conversion factor from image plane area to surface area.
\param  a_hitPos  - world position point we are going to connect with camera in LT
\param  a_hitNorm - normal of the  point we are going to connect with camera in LT
\param  a_globals - engine globals
\param  pCamDir   - out parameter. Camera forward direction.
\param  pZDepth   - out parameter. Distance between a_hitPos and camera position.
\return pdf conversion factor from image plane area to surface area.

*/

static inline float CameraImageToSurfaceFactor(const float3 a_hitPos, const float3 a_hitNorm, __global const EngineGlobals* a_globals,
                                               __private float3* pCamDir, __private float* pZDepth)
{
  const float4x4 mWorldViewInv  = make_float4x4(a_globals->mWorldViewInverse);
  const float3   camPos         = mul(mWorldViewInv, make_float3(0, 0, 0));
  const float3   camForward     = make_float3(a_globals->camForward[0], a_globals->camForward[1], a_globals->camForward[2]);
  const float    imagePlaneDist = a_globals->imagePlaneDist;

  const float  zDepth = length(camPos - a_hitPos);
  const float3 camDir = (1.0f / zDepth)*(camPos - a_hitPos); // normalize

  (*pCamDir) = camDir;
  (*pZDepth) = zDepth;

  // Compute pdf conversion factor from image plane area to surface area
  //
  const float cosToCamera = fabs(dot(a_hitNorm, camDir)); 
  const float cosAtCamera = dot(camForward, (-1.0f)*camDir);

  const float relation = a_globals->varsF[HRT_WIDTH_F] / a_globals->varsF[HRT_HEIGHT_F];
  const float fov      = relation*fmax(a_globals->varsF[HRT_FOV_X], a_globals->varsF[HRT_FOV_Y]);
  if (cosAtCamera <= cos(fov))
    return 0.0f;

  const float imagePointToCameraDist  = imagePlaneDist / cosAtCamera;
  const float imageToSolidAngleFactor = (imagePointToCameraDist*imagePointToCameraDist) / cosAtCamera; // PdfAtoW
  const float imageToSurfaceFactor    = imageToSolidAngleFactor * cosToCamera / (zDepth*zDepth);       // PdfWtoA

  if (isfinite(imageToSurfaceFactor))
    return imageToSurfaceFactor/(relation*relation);
  else
    return 0.0f;
}

static inline float2 clipSpaceToScreenSpace(float4 a_pos, const float fw, const float fh)
{
  const float x = a_pos.x*0.5f + 0.5f;
  const float y = a_pos.y*0.5f + 0.5f;
  return make_float2(x*fw - 0.5f, y*fh - 0.5f);
}

static inline float2 worldPosToScreenSpace(float3 a_wpos, __global const EngineGlobals* a_globals)
{
  const float4 posWorldSpace  = to_float4(a_wpos, 1.0f);
  const float4 posCamSpace    = mul4x4x4(make_float4x4(a_globals->mWorldView), posWorldSpace);
  const float4 posNDC         = mul4x4x4(make_float4x4(a_globals->mProj),      posCamSpace);
  const float4 posClipSpace   = posNDC*(1.0f / fmax(posNDC.w, DEPSILON));
  const float2 posScreenSpace = clipSpaceToScreenSpace(posClipSpace, a_globals->varsF[HRT_WIDTH_F], a_globals->varsF[HRT_HEIGHT_F]);
  return posScreenSpace;
}

/**
\brief  Light Tracing "connect vertex to eye" stage. Don't trace ray and don't compute shadow. You must compute shadow outside this procedure.
\param  a_lv                 - light path vertex
\param  a_ltDepth            - number of expexted samples per pass
\param  a_mLightSubPathCount - number of total light samples
\param  a_shadowHit          - a hit of 'shadow' (i.e. surface to eye) ray from a_lv.hit.pos to camPos
\param  a_globals            - engine globals
\param  a_mltStorage         - material storage
\param  a_texStorage1        - main texture storage (color)
\param  a_texStorage2        - auxilarry texture storage (for normalmaps and other)
\param  a_ptList             - proc textures list data

\param  a_pdfArray           - pdfArea array for fwd and reverse pdf's
\param  pX                   - resulting screen X position
\param  pY                   - resulting screen Y position
\param  a_outColor           - resulting color

*/

static void ConnectEyeP(const PathVertex a_lv, int a_ltDepth, float a_mLightSubPathCount, Lite_Hit a_shadowHit,
                        __global const EngineGlobals* a_globals, __global const float4* a_mltStorage, texture2d_t a_texStorage1, texture2d_t a_texStorage2, __private const ProcTextureList* a_ptList,
                        __global PdfVertex* a_pdfArray, int* pX, int* pY, __private float3* a_outColor)
{
  float3 camDir; float zDepth;
  const float imageToSurfaceFactor = CameraImageToSurfaceFactor(a_lv.hit.pos, a_lv.hit.normal, a_globals,
                                                                &camDir, &zDepth);

  if (imageToSurfaceFactor <= 0.0f || (HitSome(a_shadowHit) && a_shadowHit.t <= zDepth))
  {
    (*pX)         = -1;
    (*pY)         = -1;
    (*a_outColor) = make_float3(0, 0, 0);
    return;
  }

  const float surfaceToImageFactor  = 1.f / imageToSurfaceFactor;

  float  pdfRevW      = 1.0f;
  float3 colorConnect = make_float3(1, 1, 1);
  {
    __global const PlainMaterial* pHitMaterial = materialAt(a_globals, a_mltStorage, a_lv.hit.matId);

    ShadeContext sc;
    sc.wp = a_lv.hit.pos;
    sc.l  = camDir;               // seems like that sc.l = camDir, see smallVCM
    sc.v  = (-1.0f)*a_lv.ray_dir; // seems like that sc.v = (-1.0f)*ray_dir, see smallVCM
    sc.n  = a_lv.hit.normal;
    sc.fn = a_lv.hit.flatNormal;
    sc.tg = a_lv.hit.tangent;
    sc.bn = a_lv.hit.biTangent;
    sc.tc = a_lv.hit.texCoord;

    BxDFResult colorAndPdf = materialEval(pHitMaterial, &sc, false, true, a_globals, a_texStorage1, a_texStorage2, a_ptList);

    colorConnect     = colorAndPdf.brdf + colorAndPdf.btdf;
    pdfRevW          = colorAndPdf.pdfRev;
  }

  // we didn't eval reverse pdf yet. Imagine light ray hit surface and we immediately connect.
  //
  const float cosCurr    = fabs(dot(a_lv.ray_dir, a_lv.hit.normal));
  const float pdfRevWP   = pdfRevW / fmax(cosCurr, DEPSILON2); // pdfW po pdfWP
  const float cameraPdfA = imageToSurfaceFactor / a_mLightSubPathCount;
  
  a_pdfArray[a_ltDepth + 0].pdfRev = (pdfRevW == 0.0f) ? -1.0f*a_lv.lastGTerm : pdfRevWP*a_lv.lastGTerm;

  a_pdfArray[a_ltDepth + 1].pdfFwd = 1.0f;
  a_pdfArray[a_ltDepth + 1].pdfRev = cameraPdfA;

  ///////////////////////////////////////////////////////////////////////////////

  // We divide the contribution by surfaceToImageFactor to convert the(already
  // divided) pdf from surface area to image plane area, w.r.t. which the
  // pixel integral is actually defined. We also divide by the number of samples
  // this technique makes, which is equal to the number of light sub-paths
  //
  const float3 sampleColor = a_lv.accColor*(colorConnect / (a_mLightSubPathCount*surfaceToImageFactor));
  (*a_outColor)    = make_float3(0, 0, 0);

  const int width  = (int)(a_globals->varsF[HRT_WIDTH_F]);
  const int height = (int)(a_globals->varsF[HRT_HEIGHT_F]);

  if (dot(sampleColor, sampleColor) > 1e-12f) // add final result to image
  {
    const float2 posScreenSpace = worldPosToScreenSpace(a_lv.hit.pos, a_globals);
    
    int x = (int)(posScreenSpace.x + 0.5f);
    int y = (int)(posScreenSpace.y + 0.5f);
    
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    
    if (x >= width)  x = width  - 1;
    if (y >= height) y = height - 1;
    
    (*pX)         = x;
    (*pY)         = y;
    (*a_outColor) = sampleColor;
  }

}

static inline float3 bsdfClamping(float3 a_val)
{
  return a_val;
  //const float maxVal = 10.0f;
  //return make_float3(fmin(a_val.x, maxVal), fmin(a_val.y, maxVal), fmin(a_val.z, maxVal));
}

/**
\brief  Shadow ray connection (camera vertex to light) stage. Don't trace ray and don't compute shadow. You must compute shadow outside of this procedure.
\param  a_cv          - camera vertex we want to connect with a light 
\param  a_camDepth    - camera trace depth equal to t;
\param  a_pLight      - light that we want to connect
\param  a_explicitSam - light sample
\param  a_lightPickProb - inverse number of visiable lights
\param  a_globals     - engine globals
\param  a_mltStorage  - materials storage 
\param  a_texStorage1 - general texture storage
\param  a_texStorage2 - aux texture storage
\param  a_ptList      - proc texture list data

\param  a_tableStorage - pdf table storage
\param  a_pdfArray     - out pdfArea array 
\return connection throughput color without shadow

#TODO: add spetial check for glossy material when connect?

*/

static float3 ConnectShadowP(PathVertex a_cv, const int a_camDepth, __global const PlainLight* a_pLight, const ShadowSample a_explicitSam, const float a_lightPickProb,
                             __global const EngineGlobals* a_globals, __global const float4* a_mltStorage, texture2d_t a_texStorage1, texture2d_t a_texStorage2, __global const float4* a_tableStorage, __private const ProcTextureList* a_ptList,
                             __global PdfVertex* a_pdfArray)
{
  const float3 shadowRayDir = normalize(a_explicitSam.pos - a_cv.hit.pos); // explicitSam.direction;
  
  __global const PlainMaterial* pHitMaterial = materialAt(a_globals, a_mltStorage, a_cv.hit.matId);
  
  ShadeContext sc;
  sc.wp = a_cv.hit.pos;
  sc.l  = shadowRayDir;
  sc.v  = (-1.0f)*a_cv.ray_dir;
  sc.n  = a_cv.hit.normal;
  sc.fn = a_cv.hit.flatNormal;
  sc.tg = a_cv.hit.tangent;
  sc.bn = a_cv.hit.biTangent;
  sc.tc = a_cv.hit.texCoord;
  
  const BxDFResult evalData = materialEval(pHitMaterial, &sc, false, false, a_globals, a_texStorage1, a_texStorage2, a_ptList);
  const float pdfFwdAt1W    = evalData.pdfRev;
  
  const float cosThetaOut1  = fmax(+dot(shadowRayDir, a_cv.hit.normal), DEPSILON);
  const float cosThetaOut2  = fmax(-dot(shadowRayDir, a_cv.hit.normal), DEPSILON);
  const bool  inverseCos    = ((materialGetFlags(pHitMaterial) & PLAIN_MATERIAL_HAVE_BTDF) != 0 && dot(shadowRayDir, a_cv.hit.normal) < -0.01f);

  const float cosThetaOut   = inverseCos ? cosThetaOut2 : cosThetaOut1;  
  const float cosAtLight    = fmax(a_explicitSam.cosAtLight, DEPSILON);
  const float cosThetaPrev  = fmax(-dot(a_cv.ray_dir, a_cv.hit.normal), DEPSILON);
  
  const float3 brdfVal      = evalData.brdf*cosThetaOut1 + evalData.btdf*cosThetaOut2;
  const float  pdfRevWP     = evalData.pdfFwd / fmax(cosThetaOut, DEPSILON);
  
  const float shadowDist    = length(a_cv.hit.pos - a_explicitSam.pos);
  const float GTerm         = cosThetaOut*cosAtLight / fmax(shadowDist*shadowDist, DEPSILON2);
  
  const LightPdfFwd lPdfFwd = lightPdfFwd(a_pLight, shadowRayDir, cosAtLight, a_globals, a_texStorage1, a_tableStorage);

  a_pdfArray[0].pdfFwd = lPdfFwd.pdfA*a_lightPickProb;
  a_pdfArray[0].pdfRev = 1.0f; // a_explicitSam.isPoint ? 0.0f : 1.0f;
  
  a_pdfArray[1].pdfFwd = (lPdfFwd.pdfW / cosAtLight)*GTerm;
  a_pdfArray[1].pdfRev = (evalData.pdfFwd == 0) ? -1.0f*GTerm : pdfRevWP*GTerm;
  
  if(a_camDepth > 1)
    a_pdfArray[2].pdfFwd = (pdfFwdAt1W == 0.0f) ? -1.0f*a_cv.lastGTerm : (pdfFwdAt1W / cosThetaPrev)*a_cv.lastGTerm;
  
  const float explicitPdfW = fmax(a_explicitSam.pdf, DEPSILON2);
  return bsdfClamping((1.0f/a_lightPickProb)*a_explicitSam.color*brdfVal / explicitPdfW);
}

/**
\brief  Connect end points in SBDPT (Stochastic connection BDPT). Don't trace ray and don't compute shadow. You must compute shadow outside of this procedure.
\param  a_lv          - light  vertex we want to connect
\param  a_cv          - camera vertex we want to connect
\param  a_spit        - light trace depth equal to s;
\param  a_depth       - total trace depth equal to s+t;

\param  a_globals     - engine globals
\param  a_mltStorage  - materials storage 
\param  a_texStorage1 - general texture storage
\param  a_texStorage2 - aux texture storage
\param  a_ptList      - proc tex list data

\param  a_pdfArray    - out pdfArea array 
\return connection throughput color without shadow

*/

static float3 ConnectEndPointsP(const PathVertex a_lv, const PathVertex a_cv, const int a_spit, const int a_depth,
                                __global const EngineGlobals* a_globals, __global const float4* a_mltStorage, texture2d_t a_texStorage1, texture2d_t a_texStorage2, __private const ProcTextureList* a_ptList,
                                __global PdfVertex* a_pdfArray)
{
  if (!a_lv.valid || !a_cv.valid)
    return make_float3(0, 0, 0);

  const float3 diff = a_cv.hit.pos - a_lv.hit.pos;
  const float dist2 = fmax(dot(diff, diff), DEPSILON2);
  const float  dist = sqrt(dist2);
  const float3 lToC = diff / dist; // normalize(a_cv.hit.pos - a_lv.hit.pos)

  float3 lightBRDF = make_float3(0,0,0);
  float  lightVPdfFwdW = 0.0f;
  float  lightVPdfRevW = 0.0f;
  float  signOfNormalL = 1.0f;
  {
    ShadeContext sc;
    sc.wp = a_lv.hit.pos;
    sc.l  = lToC;                 // try to swap them ?
    sc.v  = (-1.0f)*a_lv.ray_dir; // try to swap them ?
    sc.n  = a_lv.hit.normal;
    sc.fn = a_lv.hit.flatNormal;
    sc.tg = a_lv.hit.tangent;
    sc.bn = a_lv.hit.biTangent;
    sc.tc = a_lv.hit.texCoord;

    __global const PlainMaterial* pHitMaterial = materialAt(a_globals, a_mltStorage, a_lv.hit.matId);
    BxDFResult evalData = materialEval(pHitMaterial, &sc, false, true, /* global data --> */ a_globals, a_texStorage1, a_texStorage2, a_ptList);
    lightBRDF     = evalData.brdf + evalData.btdf;
    lightVPdfFwdW = evalData.pdfFwd;
    lightVPdfRevW = evalData.pdfRev;

    const bool underSurfaceL = (dot(lToC, a_lv.hit.normal) < -0.01f);
    if ((materialGetFlags(pHitMaterial) & PLAIN_MATERIAL_HAVE_BTDF) != 0 && underSurfaceL)
      signOfNormalL = -1.0f;
  }

  float3 camBRDF = make_float3(0, 0, 0);
  float  camVPdfRevW = 0.0f;
  float  camVPdfFwdW = 0.0f;
  float  signOfNormalC = 1.0f;
  {
    ShadeContext sc;
    sc.wp = a_cv.hit.pos;
    sc.l  = (-1.0f)*lToC;
    sc.v  = (-1.0f)*a_cv.ray_dir;
    sc.n  = a_cv.hit.normal;
    sc.fn = a_cv.hit.flatNormal;
    sc.tg = a_cv.hit.tangent;
    sc.bn = a_cv.hit.biTangent;
    sc.tc = a_cv.hit.texCoord;

    __global const PlainMaterial* pHitMaterial = materialAt(a_globals, a_mltStorage, a_cv.hit.matId);
    BxDFResult evalData = materialEval(pHitMaterial, &sc, false, false, /* global data --> */ a_globals, a_texStorage1, a_texStorage2, a_ptList);
    camBRDF       = evalData.brdf + evalData.btdf;
    camVPdfRevW   = evalData.pdfFwd;
    camVPdfFwdW   = evalData.pdfRev;

    const bool underSurfaceC = (dot((-1.0f)*lToC, a_cv.hit.normal) < -0.01f);
    if ((materialGetFlags(pHitMaterial) & PLAIN_MATERIAL_HAVE_BTDF) != 0 && underSurfaceC)
      signOfNormalC = -1.0f;
  }

  const float cosAtLightVertex      = +signOfNormalL*dot(a_lv.hit.normal, lToC);  // signOfNormalL*
  const float cosAtCameraVertex     = -signOfNormalC*dot(a_cv.hit.normal, lToC);  // signOfNormalC*

  const float cosAtLightVertexPrev  = -dot(a_lv.hit.normal, a_lv.ray_dir);
  const float cosAtCameraVertexPrev = -dot(a_cv.hit.normal, a_cv.ray_dir);

  const float GTerm = cosAtLightVertex*cosAtCameraVertex / dist2;

  if (GTerm < 0.0f) // underSurfaceL || underSurfaceC
    return make_float3(0, 0, 0);


  // calc remaining PDFs
  //
  const float lightPdfFwdWP  = lightVPdfFwdW / fmax(cosAtLightVertex,  DEPSILON);
  const float cameraPdfRevWP = camVPdfRevW   / fmax(cosAtCameraVertex, DEPSILON);

  a_pdfArray[a_spit].pdfFwd = (lightPdfFwdWP  == 0.0f) ? -1.0f*GTerm : lightPdfFwdWP*GTerm;   // let s=2,t=1 => (a_spit == s == 2)
  a_pdfArray[a_spit].pdfRev = (cameraPdfRevWP == 0.0f) ? -1.0f*GTerm : cameraPdfRevWP*GTerm;  // let s=2,t=1 => (a_spit == s == 2)

  a_pdfArray[a_spit - 1].pdfRev = (lightVPdfRevW == 0.0f) ? -1.0f*a_lv.lastGTerm : a_lv.lastGTerm*(lightVPdfRevW / fmax(cosAtLightVertexPrev, DEPSILON));

  if (a_depth > 3)
    a_pdfArray[a_spit + 1].pdfFwd = (camVPdfFwdW == 0.0f) ? -1.0f*a_cv.lastGTerm : a_cv.lastGTerm*(camVPdfFwdW / fmax(cosAtCameraVertexPrev, DEPSILON));

  const bool fwdCanNotBeEvaluated = (lightPdfFwdWP < DEPSILON2)  || (a_depth > 3 && camVPdfFwdW < DEPSILON2);
  const bool revCanNotBeEvaluated = (cameraPdfRevWP < DEPSILON2) || (lightVPdfRevW < DEPSILON2);

  if (fwdCanNotBeEvaluated && revCanNotBeEvaluated)
    return make_float3(0, 0, 0);

  //bool lessOrNan1 = (!isfinite(lightBRDF.x) || (lightBRDF.x < 0)) || (!isfinite(lightBRDF.y) || (lightBRDF.y < 0)) || (!isfinite(lightBRDF.z) || (lightBRDF.z < 0));
  //bool lessOrNan2 = (!isfinite(camBRDF.x)   || (camBRDF.x < 0)) || (!isfinite(camBRDF.y) || (camBRDF.y < 0)) || (!isfinite(camBRDF.z) || (camBRDF.z < 0));
  //
  //if (maxcomp(lightBRDF) >= 0.35f || maxcomp(camBRDF) >= 0.35f || lessOrNan1 || lessOrNan2)
  //{
  //  std::cout << lightBRDF.x << lightBRDF.y << lightBRDF.z << std::endl;
  //  std::cout << camBRDF.x   << camBRDF.y << camBRDF.z << std::endl;
  //  std::cout << "GTerm = "  << GTerm << std::endl;
  //}

  return bsdfClamping(lightBRDF*camBRDF*GTerm); // fmin(GTerm,1000.0f);
}




static inline float3 environmentColor(float3 rayDir, MisData misPrev, unsigned int flags, 
                                      __global const EngineGlobals* a_globals, 
                                      __global const float4*        a_mltStorage, 
                                      __global const float4*        a_pdfStorage, 
                                      texture2d_t                   a_shadingTexture)
{
  if (a_globals->skyLightId == -1)
    return make_float3(0, 0, 0);

  unsigned int rayBounceNum  = unpackBounceNum(flags);
  unsigned int diffBounceNum = unpackBounceNumDiff(flags);

  __global const PlainLight* pEnvLight = lightAt(a_globals, a_globals->skyLightId); // in_lights + a_globals->skyLightId;

  float3 envColor = skyLightGetIntensityTexturedENV(pEnvLight, rayDir, a_globals, a_pdfStorage, a_shadingTexture);

  // //////////////////////////////////////////////////////////////////////////////////////////////

  if (rayBounceNum > 0 && !(a_globals->g_flags & HRT_STUPID_PT_MODE) && (misPrev.isSpecular == 0))
  {
    float lgtPdf    = lightPdfSelectRev(pEnvLight)*skyLightEvalPDF(pEnvLight, make_float3(0, 0, 0), rayDir, a_globals, a_pdfStorage);
    float bsdfPdf   = misPrev.matSamplePdf;
    float misWeight = misWeightHeuristic(bsdfPdf, lgtPdf); // (bsdfPdf*bsdfPdf) / (lgtPdf*lgtPdf + bsdfPdf*bsdfPdf);

    envColor *= misWeight;
  }
  
  if (misPrev.prevMaterialOffset >= 0)
  {
    __global const PlainMaterial* pPrevMaterial = materialAtOffset(a_mltStorage, misPrev.prevMaterialOffset);                            // in_plainData + misPrev.prevMaterialOffset;
    bool disableCaustics = (diffBounceNum > 0) && !(a_globals->g_flags & HRT_ENABLE_PT_CAUSTICS) && materialCastCaustics(pPrevMaterial); // and prev material cast caustics
    if (disableCaustics)
      envColor = make_float3(0, 0, 0);
  }

  // \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

  return envColor;
}


#endif
