#include <stdio.h>

static unsigned helper1(unsigned pa, unsigned pb)
{
    unsigned lr = pa ^ (pb * 3u);
    if ((unsigned)(((unsigned)(pa ^ pb) %
                    ((unsigned)(237754370u - pa) | 1u))) & 1u)
        lr += 2033554320u;
    if ((unsigned)((((unsigned)(78947964u % (pb | 1u))) & 1u)
                    ? (unsigned)(lr - pb) : pb) & 1u)
        lr += (unsigned)((pb & 1u) ? (lr << (36819021u & 31u))
                                  : 540348361u);
    lr = lr ^ pb;
    if ((unsigned)(((unsigned)(4011885469u >> (pb & 31u)) -
                    2468097618u)) & 1u)
        lr += lr;
    lr = (unsigned)(~((unsigned)(((unsigned)(3702492878u >=
                    (2572980485u ^ lr))) & (unsigned)(pb << (lr & 31u)))));
    return (unsigned)(4000493867u >= ((337123022u ^ 4164352461u) ^ lr)) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
    unsigned lr = pa ^ (pb * 3u);
    lr = (unsigned)lr;
    lr = (unsigned)(~((unsigned)lr | 0u));
    if ((unsigned)(2775648195u * 348891972u) & 1u)
        lr += lr;
    return helper1((unsigned)((2918863127u % (3575562667u | 1u)) ==
                   ((unsigned)(~((unsigned)lr | 0u)) ^ lr)),
                   (unsigned)((1685156133u << (pb & 31u)) /
                   (((unsigned)(3980704918u != (lr ^ lr))) | 1u))) ^ lr;
}

int main(void)
{
    printf("checksum=%08x\n", helper2(2216340313u, 38177487u));
    return 0;
}
