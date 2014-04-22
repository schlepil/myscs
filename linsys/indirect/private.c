#include "private.h"
#include "../common.h"
#include "linAlg.h"

#define CG_BEST_TOL 1e-7
#define PRINT_INTERVAL 100

/*y = (RHO_X * I + A'A)x */
static void matVec(Data * d, Priv * p, const pfloat * x, pfloat * y);
static idxint pcg(Data *d, Priv * p, const pfloat *s, pfloat * b, idxint max_its, pfloat tol);
static void transpose(Data * d, Priv * p);

static idxint totCgIts;
static timer linsysTimer;
static pfloat totalSolveTime;

char * getLinSysMethod(Data * d, Priv * p) {
	char * str = scs_malloc(sizeof(char) * 128);
	sprintf(str, "sparse-indirect, nnz in A = %li, CG tol ~ 1/iter^(%2.2f)", (long ) d->A->p[d->n], d->CG_RATE);
	return str;
}

char * getLinSysSummary(Priv * p, Info * info) {
	char * str = scs_malloc(sizeof(char) * 128);
	sprintf(str, "\tLin-sys: avg # CG iterations: %2.2f, avg solve time: %1.2es\n",
			(pfloat ) totCgIts / (info->iter + 1), totalSolveTime / (info->iter + 1) / 1e3);
	totCgIts = 0;
	totalSolveTime = 0;
	return str;
}

/* M = inv ( diag ( RHO_X * I + A'A ) ) */
void getPreconditioner(Data *d, Priv *p) {
	idxint i;
	pfloat * M = p->M;
	AMatrix * A = d->A;

#ifdef EXTRAVERBOSE
	scs_printf("getting pre-conditioner\n");
#endif

	for (i = 0; i < d->n; ++i) {
		M[i] = 1 / (d->RHO_X + calcNormSq(&(A->x[A->p[i]]), A->p[i + 1] - A->p[i]));
		/* M[i] = 1; */
	}

#ifdef EXTRAVERBOSE
	scs_printf("finished getting pre-conditioner\n");
#endif

}

Priv * initPriv(Data * d) {
	AMatrix * A = d->A;
	Priv * p = scs_calloc(1, sizeof(Priv));
	p->p = scs_malloc((d->n) * sizeof(pfloat));
	p->r = scs_malloc((d->n) * sizeof(pfloat));
	p->Ap = scs_malloc((d->n) * sizeof(pfloat));
	p->tmp = scs_malloc((d->m) * sizeof(pfloat));

	/* preconditioner memory */
	p->z = scs_malloc((d->n) * sizeof(pfloat));
	p->M = scs_malloc((d->n) * sizeof(pfloat));

	p->Ati = scs_malloc((A->p[d->n]) * sizeof(idxint));
	p->Atp = scs_malloc((d->m + 1) * sizeof(idxint));
	p->Atx = scs_malloc((A->p[d->n]) * sizeof(pfloat));
	transpose(d, p);
	getPreconditioner(d, p);
	totalSolveTime = 0;
	totCgIts = 0;
	if (!p->p || !p->r || !p->Ap || !p->tmp || !p->Ati || !p->Atp || !p->Atx) {
		freePriv(p);
		return NULL;
	}
	return p;
}

static void transpose(Data * d, Priv * p) {
	idxint * Ci = p->Ati;
	idxint * Cp = p->Atp;
	pfloat * Cx = p->Atx;
	idxint m = d->m;
	idxint n = d->n;

	idxint * Ap = d->A->p;
	idxint * Ai = d->A->i;
	pfloat * Ax = d->A->x;

	idxint i, j, q, *z, c1, c2;
#ifdef EXTRAVERBOSE
	timer transposeTimer;
	scs_printf("transposing A\n");
	tic(&transposeTimer);
#endif

	z = scs_calloc(m, sizeof(idxint));
	for (i = 0; i < Ap[n]; i++)
		z[Ai[i]]++; /* row counts */
	cs_cumsum(Cp, z, m); /* row pointers */

#ifdef OPENMP
#pragma omp parallel for private(i,c1,c2,q)
#endif
	for (j = 0; j < n; j++) {
		c1 = Ap[j];
		c2 = Ap[j + 1];
		for (i = c1; i < c2; i++) {
			Ci[q = z[Ai[i]]++] = j; /* place A(i,j) as entry C(j,i) */
			Cx[q] = Ax[i];
		}
	}
	scs_free(z);

#ifdef EXTRAVERBOSE
	scs_printf("finished transposing A, time: %6f s\n", tocq(&transposeTimer) / 1e3);
#endif

}

void freePriv(Priv * p) {
	if (p) {
		if (p->p)
			scs_free(p->p);
		if (p->r)
			scs_free(p->r);
		if (p->Ap)
			scs_free(p->Ap);
		if (p->tmp)
			scs_free(p->tmp);
		if (p->Ati)
			scs_free(p->Ati);
		if (p->Atx)
			scs_free(p->Atx);
		if (p->Atp)
			scs_free(p->Atp);
		if (p->z)
			scs_free(p->z);
		if (p->M)
			scs_free(p->M);
		scs_free(p);
	}
}

void solveLinSys(Data *d, Priv * p, pfloat * b, const pfloat * s, idxint iter) {
	idxint cgIts;
	pfloat cgTol = calcNorm(b, d->n) * (iter < 0 ? CG_BEST_TOL : 1 / POWF(iter + 1, d->CG_RATE));

#ifdef EXTRAVERBOSE
	scs_printf("solving lin sys\n");
#endif

	cgTol = MAX(cgTol, CG_BEST_TOL);
	tic(&linsysTimer);
	/* solves Mx = b, for x but stores result in b */
	/* s contains warm-start (if available) */
	accumByAtrans(d, p, &(b[d->n]), b);
	/* solves (I+A'A)x = b, s warm start, solution stored in b */
	cgIts = pcg(d, p, s, b, d->n, cgTol);
	scaleArray(&(b[d->n]), -1, d->m);
	accumByA(d, p, b, &(b[d->n]));

#ifdef EXTRAVERBOSE
	scs_printf("\tCG iterations: %i\n", (int) cgIts);
#endif
	if (iter >= 0) {
		totCgIts += cgIts;
	}

	totalSolveTime += tocq(&linsysTimer);
}

static void applyPreConditioner(pfloat * M, pfloat * z, pfloat * r, idxint n, pfloat *ipzr) {
	idxint i;
	*ipzr = 0;
	for (i = 0; i < n; ++i) {
		z[i] = r[i] * M[i];
		*ipzr += z[i] * r[i];
	}
}

static idxint pcg(Data *d, Priv * pr, const pfloat * s, pfloat * b, idxint max_its, pfloat tol) {
	/* solves (I+A'A)x = b */
	/* warm start cg with s */
	idxint i, n = d->n;
	pfloat ipzr, ipzrOld, alpha;
	pfloat *p = pr->p; /* cg direction */
	pfloat *Ap = pr->Ap; /* updated CG direction */
	pfloat *r = pr->r; /* cg residual */
	pfloat *z = pr->z; /* for preconditioning */
	pfloat *M = pr->M; /* inverse diagonal preconditioner */

	if (s == NULL) {
		memcpy(r, b, n * sizeof(pfloat));
		memset(b, 0.0, n * sizeof(pfloat));
	} else {
		matVec(d, pr, s, r);
		addScaledArray(r, b, n, -1);
		scaleArray(r, -1, n);
		memcpy(b, s, n * sizeof(pfloat));
	}
	applyPreConditioner(M, z, r, n, &ipzr);
	memcpy(p, z, n * sizeof(pfloat));

	for (i = 0; i < max_its; ++i) {
		matVec(d, pr, p, Ap);

		alpha = ipzr / innerProd(p, Ap, n);
		addScaledArray(b, p, n, alpha);
		addScaledArray(r, Ap, n, -alpha);

		if (calcNorm(r, n) < tol) {
			/*scs_printf("tol: %.4e, resid: %.4e, iters: %i\n", tol, rsnew, i+1); */
			return i + 1;
		}
		ipzrOld = ipzr;
		applyPreConditioner(M, z, r, n, &ipzr);

		scaleArray(p, ipzr / ipzrOld, n);
		addScaledArray(p, z, n, 1);
	}
	return i;
}

/*y = (RHO_X * I + A'A)x */
static void matVec(Data * d, Priv * p, const pfloat * x, pfloat * y) {
	pfloat * tmp = p->tmp;
	memset(tmp, 0, d->m * sizeof(pfloat));
	accumByA(d, p, x, tmp);
	memset(y, 0, d->n * sizeof(pfloat));
	accumByAtrans(d, p, tmp, y);
	addScaledArray(y, x, d->n, d->RHO_X);
}

void _accumByAtrans(idxint n, pfloat * Ax, idxint * Ai, idxint * Ap, const pfloat *x, pfloat *y) {
	/* y  = A'*x
	 A in column compressed format
	 parallelizes over columns (rows of A')
	 */
	idxint p, j;
	idxint c1, c2;
	pfloat yj;
#ifdef OPENMP
#pragma omp parallel for private(p,c1,c2,yj)
#endif
	for (j = 0; j < n; j++) {
		yj = y[j];
		c1 = Ap[j];
		c2 = Ap[j + 1];
		for (p = c1; p < c2; p++) {
			yj += Ax[p] * x[Ai[p]];
		}
		y[j] = yj;
	}
}

void accumByAtrans(Data * d, Priv * p, const pfloat *x, pfloat *y) {
	AMatrix * A = d->A;
	_accumByAtrans(d->n, A->x, A->i, A->p, x, y);
}
void accumByA(Data * d, Priv * p, const pfloat *x, pfloat *y) {
	_accumByAtrans(d->m, p->Atx, p->Ati, p->Atp, x, y);
}
