/* SPDX-License-Identifier: Apache-2.0
 * Semantic model of the PIE batch, not a performance implementation.
 * For a wide sum x=4h+l, h is signed16 and0<=l<=3. For P=h*W and L=l*W:
 * floor((4P+L+b)/2^S) = (P>>15)*2^(17-S)
 *   + (((P&32767)+(L>>2)+(b>>2))>>(S-2)).
 * S=14..17, b is0 or a multiple of4. P>>15 fits signed16 under Q14 L1<=23172;
 * L>>2 also fits signed16. CMUL SAR0 retains P mod65536, masked to15 bits.
 * The final sum is computed in signed32 then saturated, never wrapped.
 */
#include "gmf_radix4_simd.h"
static int16_t sat(int32_t v){return v>32767?32767:v< -32768?-32768:(int16_t)v;}
static int32_t product(int32_t r,int32_t i,int32_t wr,int32_t wi,int s,int bias) {
    const int32_t h=(r>>2)*wr+(i>>2)*wi,l=(r&3)*wr+(i&3)*wi;
    const int16_t coarse=(int16_t)(h>>15),low=(int16_t)h,residual=(int16_t)(l>>2);
    return (int32_t)coarse*(1<<(17-s))+(((low&32767)+residual+bias)>>(s-2));
}
void gmf_r4_batch_model(const gmf_r4_batch* c,int inverse) {
    int16_t out[4][8];int sign=inverse?-1:1,s=c->product_shift-14;
    for(int j=0;j<4;++j) {
        int32_t ar=c->stream[0][2*j],ai=c->stream[0][2*j+1];
        int32_t br=c->stream[1][2*j],bi=c->stream[1][2*j+1];
        int32_t cr=c->stream[2][2*j],ci=c->stream[2][2*j+1];
        int32_t dr=c->stream[3][2*j],di=c->stream[3][2*j+1];
        int32_t r[4]={ar+br+cr+dr,ar-br+cr-dr,ar-cr+sign*(bi-di),ar-cr-sign*(bi-di)};
        int32_t i[4]={ai+bi+ci+di,ai-bi+ci-di,ai-ci-sign*(br-dr),ai-ci+sign*(br-dr)};
        int rounding=c->bias_quarter>>12;
        out[0][2*j]=sat((r[0]+rounding)>>s);out[0][2*j+1]=sat((i[0]+rounding)>>s);
        for(int a=1;a<4;++a){int windex=a==1?1:a==2?0:2;
            int32_t wr=c->twiddle[windex][2*j],wi=sign*c->twiddle[windex][2*j+1];
            out[a][2*j]=sat(product(r[a],i[a],wr,wi,c->product_shift,c->bias_quarter));
            out[a][2*j+1]=sat(product(i[a],r[a],wr,-wi,c->product_shift,c->bias_quarter));}
    }
    for(int a=0;a<4;++a)for(int j=0;j<8;++j)c->stream[a][j]=out[a][j];
}
void gmf_r4_real_pairs_model(const int16_t* a,const int16_t* b,const int16_t* w,int32_t* out,int inverse,int bias_quarter) {
    for(int j=0;j<4;++j){int32_t ar=a[2*j],ai=a[2*j+1],br=b[2*j],bi=b[2*j+1];
        int32_t sr=ar+br,si=ai-bi,dr=ar-br,di=ai+bi,wr=w[2*j],wi=(inverse?-1:1)*w[2*j+1];
        int32_t tr=product(dr,di,wr,-wi,14,bias_quarter),ti=product(di,dr,wr,wi,14,bias_quarter);
        out[j]=sr+tr;out[4+j]=si+ti;out[8+j]=sr-tr;out[12+j]=ti-si;}
}
