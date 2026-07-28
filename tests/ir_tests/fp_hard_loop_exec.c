float dot(float *a, float *b, int n){ float s=0.0f; for(int i=0;i<n;i++) s += a[i]*b[i]; return s; }
float poly(float x){ float r=0.0f; for(int i=0;i<5;i++){ r = r*x + (float)i; } return r; }
int cmpchain(float a, float b, float c){ if(a<b && b<c) return 1; if(a>=c) return 2; return 3; }
float mixarr(int n){ float acc=0.0f; for(int i=0;i<n;i++){ acc += (float)i * 0.5f; } return acc; }
int main(void){
  float a[3]={1.0f,2.0f,3.0f}, b[3]={4.0f,5.0f,6.0f};
  int r=0;
  if((int)dot(a,b,3)!=32) r|=1;      /* 4+10+18=32 */
  if((int)poly(2.0f)!=26) r|=2;      /* ((((0*2+0)*2+1)*2+2)*2+3)*2+4 = 26 */
  if(cmpchain(1.0f,2.0f,3.0f)!=1) r|=4;
  if((int)mixarr(4)!=3) r|=8;        /* 0+0.5+1+1.5=3.0 */
  return r? (100+r):1;
}
