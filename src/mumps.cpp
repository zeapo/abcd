#include "abcd.h"
void abcd::initializeMumps()
{
    initializeMumps(false);
}

void abcd::initializeMumps(bool local)
{
    mpi::communicator world;
    // The first run of MUMPS is local to CG-masters
    std::vector<int> r;
    if(local) {
        r = { inter_comm.rank() };
        mpi::group grp = inter_comm.group().include(r.begin(), r.end());
        intra_comm = mpi::communicator(inter_comm, grp);
    } else {
        if(instance_type == 0) {
            r.push_back(world.rank());
        } else {
            r.push_back(my_master);
        }
        std::copy(my_slaves.begin(), my_slaves.end(), std::back_inserter(r));
        
        mpi::group grp = world.group().include(r.begin(), r.end());
        intra_comm = mpi::communicator(world, grp);
    }

    mumps.sym = 2;
    mumps.par = 1;
    mumps.job = -1;
    mumps.comm_fortran = MPI_Comm_c2f((MPI_Comm) intra_comm);

    dmumps_c(&mumps);
    if(getMumpsInfo(1) != 0) throw - 100 + getMumpsInfo(1);

    setMumpsIcntl(1, -1);
    setMumpsIcntl(2, -1);
    setMumpsIcntl(3, -1);

    setMumpsIcntl(14, 50);
    setMumpsIcntl(12, 2);
    setMumpsIcntl(6, 5);
    setMumpsIcntl(7, 5);
    setMumpsIcntl(8, -2);
    setMumpsIcntl(27, 16);
}

void abcd::createAugmentedSystems()
{
    SparseMatrix<double, RowMajor> G;
    m_n = 0;
    m_nz = 0;
    // for performance, compute total nnz and size of the matrix
    for(int j = 0; j < parts.size(); j++) {
        m_n += parts[j].cols() + parts[j].rows();
        m_nz += parts[j].cols() + parts[j].nonZeros();
    }

    // Allocate the data for mumps
    mumps.n = m_n;
    mumps.nz = m_nz;
    mumps.irn = new int[m_nz];
    mumps.jcn = new int[m_nz];
    mumps.a = new double[m_nz];

    // Use Fortran array => start from 1
    int i_pos = 1;
    int j_pos = 1;
    int st = 0;

    for(int p = 0; p < parts.size(); p++) {

        // fill the identity
        for(int i = 0; i < parts[p].cols(); i++) {
            mumps.irn[st + i] = i_pos + i;
            mumps.jcn[st + i] = j_pos + i;
            mumps.a[st + i] = 1;
        }

        // we get down by nb_cols
        i_pos += parts[p].cols();
        // we added nb_cols elements
        st += parts[p].cols();

        for(int k = 0; k < parts[p].rows(); k++) {
            for(int j = parts[p].outerIndexPtr()[k]; j < parts[p].outerIndexPtr()[k + 1]; j++) {
                mumps.irn[st] = i_pos + k;
                mumps.jcn[st] = j_pos + parts[p].innerIndexPtr()[j];
                mumps.a[st] = parts[p].valuePtr()[j];
                st++;
            }
        }

        i_pos += parts[p].rows();
        j_pos += parts[p].cols() + parts[p].rows();

    }
//     The data given to MUMPS
//     for(int k = 0; k < st; k++) {
//         cout << mumps.irn[k] << " ";
//         cout << mumps.jcn[k] << " ";
//         cout << mumps.a[k] << endl;
//     }
//
//     cout << l_nnz << " " << st << endl;
//     cout << l_n<< " " << i_pos << endl;
}

void abcd::allocateMumpsSlaves()
{
    mpi::communicator world;

    if(instance_type == 0) {
        std::vector<int> flops(inter_comm.size());
        std::vector<dipair> flops_s;
        std::vector<int> slaves_for_me(inter_comm.size());
        std::vector<int> slaves_for_me_t(inter_comm.size());
        mpi::all_gather(inter_comm, (int) getMumpsRinfo(1), flops);

        for(int idx = 0; idx < inter_comm.size(); idx++) {
            flops_s.push_back(dipair(flops[idx], idx));
        }

        std::sort(flops_s.begin(), flops_s.end(), ip_comp);


        int s = std::accumulate(flops.begin(), flops.end(), 0);
        std::vector<double> shares;
        int nb_slaves = world.size() - inter_comm.size();
        int slaves_left = nb_slaves;
        double top = 1, low = 0.90;

        for(int i = 0; i < inter_comm.size() && slaves_left > 0 ; i++) {
            shares.push_back(((double)flops_s[i].first / (double) s) * nb_slaves);
        }

        while(slaves_left > 0) {
            for(int i = 0; i < inter_comm.size() && slaves_left > 0 ; i++) {
                int share_of_slaves = 0;
                if(shares[i] < 0) continue;
                if((shares[i] - floor(shares[i])) >= low  &&
                        (shares[i] - floor(shares[i])) < top) {
                    share_of_slaves = ceil(shares[i]) < slaves_left ? ceil(shares[i]) : slaves_left;
                } else {
                    share_of_slaves = floor(shares[i]) < slaves_left ? floor(shares[i]) : slaves_left;
                }
                slaves_for_me_t[i] += share_of_slaves;
                slaves_left -= share_of_slaves;
                shares[i] -= share_of_slaves;
            }
            top -= 0.10;
            low -= 0.10;
        }
        for(int idx = 0; idx < inter_comm.size(); idx++) {
            slaves_for_me[ flops_s[idx].second ] = slaves_for_me_t[idx];
        }

        int current_slave = inter_comm.size();
        for(int your_master = 0 ; your_master < inter_comm.size(); your_master++) {

            for(int i = 0; i < slaves_for_me[your_master]; i++) {

                // let the master handle this!
                if(inter_comm.rank() == 0)
                    world.send(current_slave, 11,  your_master);

                if(inter_comm.rank() == your_master) {
                    my_slaves.push_back(current_slave);
                }

                current_slave++;
            }
        }
        // Now that the slaves know who's their daddy, tell who are their brothers
        for(std::vector<int>::iterator slave = my_slaves.begin(); slave != my_slaves.end(); slave++) {
            world.send(*slave, 12, my_slaves);
        }

    } else {
        world.recv(0, 11, my_master);
        //Store in my_slaves my brothers in slavery
        world.recv(my_master, 12, my_slaves);
    }
}

void abcd::analyseAugmentedSystems()
{
    mumps.job = 1;

    double t = MPI_Wtime();
    dmumps_c(&mumps);
    t = MPI_Wtime() - t;

    if(getMumpsInfo(1) != 0) throw - 10000 + 100 * getMumpsInfo(1) - getMumpsInfo(2);

    if(instance_type == 0) {
        double flop = getMumpsRinfo(1);
        int prec = cout.precision();
        cout.precision(2);
        cout << string(32, '-') << endl
             << "| MUMPS ANALYSIS on MA " << setw(7) << inter_comm.rank() << " |" << endl
             << string(32, '-') << endl
             << "| Flops estimate: " << setw(6) << scientific << flop << string(4, ' ') << " |" << endl
             << "| Time:           " << setw(6) << t << " sec |" << endl
             << string(32, '-') << endl;
        cout.precision(prec);
    }
}


void abcd::factorizeAugmentedSystems()
{
    mpi::communicator world;
    mumps.job = 2;

    double t = MPI_Wtime();
    dmumps_c(&mumps);
    t = MPI_Wtime() - t;

    if(getMumpsInfo(1) != 0) throw - 10000 + 100 * getMumpsInfo(1) - getMumpsInfo(2);

    if(instance_type == 0) {
        double flop = getMumpsRinfo(1);
        int prec = cout.precision();
        cout.precision(2);
        cout << string(32, '-') << endl
             << "| MUMPS FACTORIZ on MA " << setw(7) << inter_comm.rank() << " |" << endl
             << string(32, '-') << endl
             << "| Flops estimate: " << setw(6) << scientific << flop << string(4, ' ') << " |" << endl
             << "| Time:           " << setw(6) << t << " sec |" << endl
             << string(32, '-') << endl;
        cout.precision(prec);
    }
}


