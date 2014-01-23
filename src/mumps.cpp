#include <abcd.h>




/* 
 * ===  FUNCTION  ======================================================================
 *         Name:  abcd::waitForSolve
 *  Description:  
 * =====================================================================================
 */
    void
abcd::waitForSolve()
{
    DMUMPS_STRUC_C mu;
    mpi::communicator world;

    int job = 0;
    do{
        mpi::broadcast(intra_comm, job, 0);
        
        if(job == -1) break;
        if(job == 1){
            mumps.job = 3;
            dmumps_c(&mumps);
        } else if (job == 2) {
            mu.sym = 2;
            mu.par = 1;
            mu.job = -1;

            mu.comm_fortran = MPI_Comm_c2f((MPI_Comm) world);
            dmumps_c(&mu);

            mu.icntl[0] = -1;
            mu.icntl[1] = -1;
            mu.icntl[2] = -1;

            mu.job = 1;
            dmumps_c(&mu);
            mu.job = 2;
            dmumps_c(&mu);
            mu.job = 3;
            dmumps_c(&mu);
        }
    }while(true);

}		/* -----  end of function abcd::waitForSolve()  ----- */



MV_ColMat_double abcd::spSimpleProject(std::vector<int> mycols)
{
    bool dense_rhs = (icntl[13] == 1);
    //dense_rhs = true;

    int s = mycols.size();
    // Build the mumps rhs

    mumps.rhs = new double[mumps.n * s];
    for(int i = 0; i < mumps.n * s; i++) mumps.rhs[i] = 0;

    MV_ColMat_double mumps_rhs(mumps.rhs, mumps.n, s, MV_Matrix_::ref);

    CompCol_Mat_double mumps_comp_rhs;

    std::vector<int> rr;
    std::vector<double> rv;

    std::vector<CompRow_Mat_double> r;
    std::vector<std::map<int,int> > loc_cols(partitions.size());

    int nzr_estim = 0;

    for(int k = 0; k < partitions.size(); k++) {

        CompRow_Mat_double Y;

        VECTOR_int yr(mycols.size(), 0);
        VECTOR_int yc(mycols.size(), 0);
        VECTOR_double yv(mycols.size(), 0);
        int c;

        int ct = 0;
        for(int i = 0; i < mycols.size(); i++){
            c = mycols[i];
            if(glob_to_part[k].find(n_o + c) != glob_to_part[k].end()){
                yr[ct] = glob_to_part[k][n_o + c];
                yc[ct] = i;
                yv[ct] = 1;

                ct++;
                loc_cols[k][i] = 1;
            }
        }

        Coord_Mat_double Yt(partitions[k].dim(1), s, ct, yv.ptr(), yr.ptr(), yc.ptr());

        Y = CompRow_Mat_double(Yt);

        nzr_estim += Y.NumNonzeros();

        r.push_back( spmm(partitions[k], Y) );
    }


    if(dense_rhs){
        // dense mumps rhs
        mumps.icntl[20 - 1] = 0;

        int pos = 0;
        for(int k = 0; k < partitions.size(); k++) {
            for(int r_p = 0; r_p < s; r_p++) {
                for(int i = pos; i < pos + partitions[k].dim(1); i++) {
                    mumps_rhs(i, r_p) = 0;
                }
                int j = 0;
                for(int i = pos + partitions[k].dim(1); i < pos + partitions[k].dim(1) + partitions[k].dim(0); i++) {
                    mumps_rhs(i, r_p) = r[k](j, r_p);
                    j++;
                }
            }
            pos += partitions[k].dim(1) + partitions[k].dim(0);
        }
    } else {
        // sparse mumps rhs
        mumps.icntl[20 - 1] = 1;

        {
            mumps.irhs_ptr      = new int[s + 1];
            rr.reserve(nzr_estim);
            rv.reserve(nzr_estim);

            int cnz = 1;

			int pos = 0;
			for(int k = 0; k < partitions.size(); k++) {
				CompRow_Mat_double rtt = r[k];
				int _dim1 = partitions[k].dim(1);
				int _dim0 = partitions[k].dim(0);

				for(int r_p = 0; r_p < s; r_p++) {
					mumps.irhs_ptr[r_p] = cnz;

                    // no need to put zeros before!
                    //
                    int j = 0;
                    for(int i = pos + _dim1; i < pos + _dim1 + _dim0; i++) {
                        if(rtt(j, r_p) != 0){
                            rr.push_back(i+1);
                            rv.push_back(rtt(j, r_p));
                            cnz++;
                        }
                        j++;
                    }
                }
				pos += _dim1 + _dim0;
            }
            mumps.irhs_ptr[s] = cnz;

            mumps.nz_rhs        = cnz - 1;

            mumps.rhs_sparse    = &rv[0];
            mumps.irhs_sparse   = &rr[0];
        }

    }

    int job = 1;
    mpi::broadcast(intra_comm, job, 0);

    mumps.nrhs          = s;
    mumps.lrhs          = mumps.n;
    mumps.job           = 3;

    dmumps_c(&mumps);

    MV_ColMat_double Delta(size_c, s, 0);

    int dlda = Delta.lda();
    double *dpt = Delta.ptr();

    int x_pos = 0;

    for(int k = 0; k < partitions.size(); k++) {
        int start_c = glob_to_part[k][stC[k]];
        x_pos += glob_to_part[k][stC[k]];

        for(int i = start_c; i < column_index[k].size(); i++){

            int ci = column_index[k][i] - n_o;

            for(int j = 0; j < s; j++) {
                //Delta(ci, j) = Delta(ci, j) - mumps_rhs(x_pos, j) ; // Delta = - \sum (sol)

                dpt[ci + j * dlda] -= mumps.rhs[x_pos + j * mumps.n];
            }

            x_pos++;
        }

        for(int j = 0; j < s; j++) {
            if(loc_cols[k][j]){
                int c = mycols[j];

                //Delta(c, j) = 0.5 + Delta(c,j);
                dpt[c + j * dlda] += 0.5;
            }
        }

        x_pos += partitions[k].dim(0);
    }

    // disable sparse mumps rhs
    mumps.icntl[20 - 1] = 0;

    //delete mumps.rhs;
    delete[] mumps.rhs;

    return Delta;
}
