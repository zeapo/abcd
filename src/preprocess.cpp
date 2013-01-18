#include <abcd.h>

extern "C"
{
    void mc77id_(int *, double *);
    void mc77ad_(int *job, int *m, int *n, int *nnz, int *jcst, int *irn, double *a,
                 int *iw, int *liw, double *dw, int *ldw, int *icntl, double *cntl,
                 int *info, double *rinfo);
}


void abcd::preprocess()
{
    if(icntl[9] > 0) {
        drow_ = VECTOR_double(m, double(1));
        dcol_ = VECTOR_double(n, double(1));
    }

    if(icntl[9] == 1) {
        ///TODO:Use logging
        std::cout << "[-] Scaling with Infinity" << std::endl;

        abcd::scaleMatrix(0);

        double rsum;
        drow_ = VECTOR_double(m, double(0));
        dcol_ = VECTOR_double(n, double(1));
        for(int r = 0; r < m; r++) {
            rsum = 0;
            for (int c=A.row_ptr(r); c<A.row_ptr(r+1); c++){
                rsum += pow(A(r, A.col_ind(c)), 2);
            }
            drow_(r) = sqrt(rsum);
        }
        abcd::diagScaleMatrix(drow_, dcol_);
    }


    if(icntl[9] == 2) {
        std::cout << "[-] Scaling with Norm 1" << std::endl;
        abcd::scaleMatrix(1);

        std::cout << "[-] Scaling with Norm 2" << std::endl;
        abcd::scaleMatrix(2);
    }

}

void abcd::scaleMatrix(int norm)
{
    int ldw, liw, nout, job;
    int *iw;
    double *dw;
    int mc77_icntl[10], mc77_info[10];
    double mc77_dcntl[10], mc77_rinfo[10];

    mc77id_(mc77_icntl, mc77_dcntl);

    if(mc77_icntl[4] == 0)
        ldw = nz;
    else
        ldw = 0;

    if(mc77_icntl[5] == 0) {
        liw = n * 2;
        ldw = ldw + 4 * n;
    } else {
        liw = n;
        ldw = ldw + 2 * n;
    }

    liw = liw * 2;
    ldw = ldw * 2;

    iw = new int[liw];
    dw = new double[ldw];

    // Increment indices to call fortran code
    for(int k = 0; k < n + 1 ; k++) {
        A.rowptr_.t_vec()[k]++;
    }
    for(int k = 0; k < nz ; k++) {
        A.colind_.t_vec()[k]++;
    }

    if(norm < 0) throw - 12;

    mc77_icntl[9] = 0;
    //if(norm == 2) mc77_icntl[6] = 20;

    job = norm;
    mc77ad_(&job, &m, &n, &nz, A.rowptr_.t_vec(), A.colind_.t_vec(), A.val_.t_vec(),
            iw, &liw, dw, &ldw, mc77_icntl, mc77_dcntl, mc77_info, mc77_rinfo);

    if(mc77_info[0] < 0) throw - 100 + mc77_info;

    if(norm == 0)
        cout << "Distance from 1 (norm inf) : " << mc77_rinfo[0] << endl;
    else
        cout << "Distance from 1 (norm " << norm << ") : " << mc77_rinfo[0] << endl;

    // put them back to 0-based for C/C++
    for(int k = 0; k < n + 1 ; k++) {
        A.rowptr_.t_vec()[k]--;
    }
    for(int k = 0; k < nz ; k++) {
        A.colind_.t_vec()[k]--;
    }

    VECTOR_double dc(m, double(1)), dr(n, double(1));
    // Scale the matrix
    for(int k = 0; k < n; k++) {
        dc(k) = 1 / dw[k];
        dcol_(k) *= 1 / dw[k];
    }

    for(int k = 0; k < m; k++) {
        dr(k) = 1 / dw[k + n];
        drow_(k) *= 1 / dw[k + n];
    }

    diagScaleMatrix(dr, dc);


    delete iw, dw;
}


/* 
 * ===  FUNCTION  ======================================================================
 *         Name:  abcd::diagscal
 *  Description:  
 * =====================================================================================
 */
    void
abcd::diagScaleMatrix ( VECTOR_double drow, VECTOR_double dcol)
{
    int c;
    for ( int i = 0; i < m; i++ ) {
        for ( int j = A.row_ptr(i); j < A.row_ptr(i+1); j++ ) {
            A.set(i,A.col_ind(j)) = A(i,A.col_ind(j)) * drow(i) * dcol(A.col_ind(j));
        }
    }
}		/* -----  end of function abcd::diagscal  ----- */


/* 
 * ===  FUNCTION  ======================================================================
 *         Name:  abcd::scalRhs
 *  Description:  
 * =====================================================================================
 */
    void
abcd::diagScaleRhs ( VECTOR_double &b)
{
    for ( int i = 0; i < m; i++ ) {
        b(i) = b(i)*drow_(i);
    }

}		/* -----  end of function abcd::scalRhs  ----- */
