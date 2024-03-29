#include "inmost.h"

//    !!!!!!! Currently NOT suited for parallel run
//
//
//    This code solves the following
//    boundary value problem for diffusion equation
//
//    div(-D grad U) = f       in unit square
//    U              = g       on boundary
//
//    D is diffusion tensor, s.p.d. 2x2 matrix defined by 3 numbers Dxx, Dyy, Dxy
//
//    The user should provide 2D mesh
//    (preferrably, a .vtk file which can be generated by Gmsh for example)
//    which is built for (0;1)x(0;1)
//
//    The code will then
//    - process mesh,
//    - init tags,
//    - assemble linear system,
//    - solve it with INMOST inner linear solver,
//    - save solution in a .vtk file.


using namespace INMOST;
using namespace std;

enum{
    T_ASSEMBLE = 0,
    T_SOLVE,
    T_PRECOND,
    T_IO,
    T_INIT,
    T_UPDATE
};

const string tagNameTensor = "DIFFUSION_TENSOR";
const string tagNameBC     = "BOUNDARY_CONDITION";
const string tagNameRHS    = "RHS";
const string tagNameSol    = "SOLUTION";
const string tagNameSolEx  = "SOLUTION_EXACT";

const unsigned n_polys = 3;


// Corresponds to tensor
// [ 1  0 ]
// [ 0 10 ]
// rotated by M_PI/6
const double Dxx = 1;//3.25;
const double Dxy = 0;//-0.433013;
const double Dyy = 1;//0.25;

#ifndef M_PI
const double M_PI = 3.1415926535898;
#endif

double exactSolution(double *x)
{
    return x[0];//sin(M_PI*x[0]) * sin(M_PI*x[1]);
}

double exactSolutionRHS(double *x)
{
    return 0;//M_PI*M_PI * ((Dxx+Dyy) * exactSolution(x) - 2*Dxy*cos(M_PI*x[0])*cos(M_PI*x[1]));
}

class Problem
{
private:
    Mesh m;
    // List of mesh tags
    Tag tagD;     // Diffusion tensor
    Tag tagBC;    // Boundary conditions
    Tag tagSol;   // Solution
    Tag tagSolEx; // Exact solution

    MarkerType mrkDirNode;  // Dirichlet node marker
    MarkerType mrkUnknwn;

    Automatizator aut;    // Automatizator to handle all AD things
    Residual R;           // Residual to assemble
    dynamic_variable var; // Variable containing solution

    int rank; // for parallel runs

    unsigned numDirNodes;

    double times[10];
    double ttt; // global timer

public:
    Problem(string meshName);
    ~Problem();
    void initProblem(); // create tags and set parameters
    void assembleGlobalSystem(); // assemble global linear system
    rMatrix computeW(Cell &);
    rMatrix integrateRHS(Cell &);
    void assembleLocalSystem(Cell &, rMatrix &, rMatrix &);
    void solveSystem();
    void saveSolution(string path); // save mesh with solution
};

Problem::Problem(string meshName)
{
    ttt = Timer();
    for(int i = 0; i < 10; i++)
        times[i] = 0.;

    rank = m.GetProcessorRank();

    double t = Timer();
    if(rank == 0){
        m.Load(meshName);
        cout << "Number of cells: " << m.NumberOfCells() << endl;
        cout << "Number of faces: " << m.NumberOfFaces() << endl;
        cout << "Number of edges: " << m.NumberOfEdges() << endl;
        cout << "Number of nodes: " << m.NumberOfNodes() << endl;
    }
    times[T_IO] += Timer() - t;
}

Problem::~Problem()
{
    printf("\n+=========================\n");
    printf("| T_assemble = %lf\n", times[T_ASSEMBLE]);
    printf("| T_precond  = %lf\n", times[T_PRECOND]);
    printf("| T_solve    = %lf\n", times[T_SOLVE]);
    printf("| T_IO       = %lf\n", times[T_IO]);
    printf("| T_update   = %lf\n", times[T_UPDATE]);
    printf("| T_init     = %lf\n", times[T_INIT]);
    printf("+-------------------------\n");
    printf("| T_total    = %lf\n", Timer() - ttt);
    printf("+=========================\n");
}

void Problem::initProblem()
{
    double t = Timer();
    tagD     = m.CreateTag(tagNameTensor, DATA_REAL, CELL, NONE, 3);
    tagBC    = m.CreateTag(tagNameBC,     DATA_REAL, NODE, NODE, 1);
    tagSol   = m.CreateTag(tagNameSol,    DATA_REAL, NODE, NONE, 1);
    tagSolEx = m.CreateTag(tagNameSolEx,  DATA_REAL, NODE, NONE, 1);

    // Set diffusion tensor
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;

        icell->RealArray(tagD)[0] = Dxx; // Dxx
        icell->RealArray(tagD)[1] = Dyy; // Dyy
        icell->RealArray(tagD)[2] = Dxy; // Dxy
    }
    m.ExchangeData(tagD, CELL);

    // Set boundary conditions
    // Mark and count Dirichlet nodes
    // Compute RHS and exact solution
    numDirNodes = 0;
    mrkDirNode = m.CreateMarker();
    mrkUnknwn = m.CreateMarker();
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetStatus() == Element::Ghost)
            continue;

        Node node = inode->getAsNode();
        double x[2];
        node.Barycenter(x);

        node.Real(tagSolEx) = exactSolution(x);
        node.Real(tagSol) = 10;

        if(!node.Boundary()){
            node->SetMarker(mrkUnknwn);
            continue;
        }

        node.SetMarker(mrkDirNode);
        numDirNodes++;
        node.Real(tagBC)  = exactSolution(x);
    }
    cout << "Number of Dirichlet nodes: " << numDirNodes << endl;

    Automatizator::MakeCurrent(&aut);

    INMOST_DATA_ENUM_TYPE SolTagEntryIndex = 0;
    SolTagEntryIndex = aut.RegisterTag(tagSol, NODE, mrkUnknwn);
    var = dynamic_variable(aut, SolTagEntryIndex);
    aut.EnumerateEntries();
    R = Residual("fem_diffusion", aut.GetFirstIndex(), aut.GetLastIndex());
    times[T_INIT] += Timer() - t;
}

void Problem::assembleGlobalSystem()
{
    double t = Timer();
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;
        Cell cell = icell->getAsCell();

        ElementArray<Node> nodes = icell->getNodes();
        rMatrix rhs, W;
        assembleLocalSystem(cell, W, rhs);

        auto nnodes = nodes.size();

        for(unsigned i = 0; i != nnodes; i++){
            if(nodes[i]->GetMarker(mrkDirNode)){
                double bcVal = nodes[i].Real(tagBC);
                for(unsigned j = 0; j != nnodes; j++)
                    if(!nodes[j].GetMarker(mrkDirNode)){
                        R[var.Index(nodes[j])] += bcVal * W(j,i);
                    }
            }
            else{
                // Node with unknown
                for(unsigned j = 0; j != nnodes; j++)
                    if(!nodes[j].GetMarker(mrkDirNode))
                        R[var.Index(nodes[i])] += W(j,i) * var(nodes[j]);
                R[var.Index(nodes[i])] -= rhs(i,0);
            }
        }
    }
    times[T_ASSEMBLE] += Timer() - t;
}


void Problem::assembleLocalSystem(Cell &cell, rMatrix &W, rMatrix &b)
{
    auto nodes = cell.getNodes();
    auto faces = cell.getFaces();
    unsigned nn = static_cast<unsigned>(nodes.size());
    if(faces.size() != nodes.size())
        exit(1);
    double xc[2], diam = 0.;
    cell.Centroid(xc);
    for(auto it = nodes.begin(); it != nodes.end(); it++){
        auto xi = it->Coords();
        for(auto jt = nodes.begin(); jt != nodes.end(); jt++){
            auto xj = jt->Coords();
            diam = max(diam, (xi[0]-xj[0])*(xi[0]-xj[0]) + (xi[1]-xj[1])*(xi[1]-xj[1]));
        }
    }
    diam = sqrt(diam);
    //printf("Cell %d (%lf, %lf): nn = %d, diam = %e\n", cell.DataLocalID(), xc[0],xc[1], nn, diam);
    rMatrix D(nn, n_polys), B(n_polys, nn);
    B.Zero();
    D.Zero();
    for(unsigned i = 0; i < nn; i++){
        D(i,0) = 1.0;
    }
    for(unsigned i = 0; i < nn; i++){
        B(0,i) = 1.0/nn;
    }

    for(unsigned vid = 0; vid < nn; vid++){
        Node n = nodes[vid];
        unsigned indprev = vid == 0 ? nn-1 : vid-1;
        unsigned indnext = vid == nn-1 ? 0 : vid+1;
        Node prev = nodes[indprev];
        Node next = nodes[indnext];
        double nor[2];
        auto xv = n.Coords();
        auto xp = prev.Coords();
        auto xn = next.Coords();
        nor[0] = xn[1] - xp[1];
        nor[1] = xp[0] - xn[0];

        //printf("node %d (%e, %e)\n", vid, xv[0], xv[1]);
        //printf("node %d, n = (%lf, %lf)\n", vid, nor[0], nor[1]);
        //printf("node %d, prev = %d, next = %d\n", vid, indprev, indnext);

        for(unsigned i = 1; i < n_polys; i++){
            double polygrad[2];
            if(i == 1){
                polygrad[0] = 1.;
                polygrad[1] = 0.;
            }
            else if(i == 2){
                polygrad[0] = 0.;
                polygrad[1] = 1.;
            }
            rMatrix monomgrad(2,1);
            monomgrad(0,0) = polygrad[0] / diam;
            monomgrad(1,0) = polygrad[1] / diam;
            //monomgrad.Print();
            D(vid,i) = (xv[0]-xc[0])*polygrad[0] + (xv[1]-xc[1])*polygrad[1];
            D(vid,i) /= diam;
            B(i, vid) = 0.5 * (monomgrad(0,0)*nor[0] + monomgrad(1,0)*nor[1]);
        }
    }
    //B.Print();
    rMatrix Proj = (B*D).Invert() * B;
    rMatrix Se(nn,nn);
    Se.Zero();
    for(unsigned i = 0; i < nn; i++)
        Se(i,i) = 1.;
    Se = Se - D*Proj;
    Se = Se.Transpose() * Se;
    rMatrix G = B*D;
    for(unsigned i = 0; i < G.Cols(); i++)
        G(0,i) = 0;
    W = Proj.Transpose() * G * Proj + Se;

    //W.Print();
    //exit(1);
    b = rMatrix(nn,1);
    double rhs = exactSolutionRHS(xc) * cell.Volume() / nn;
    for(unsigned i = 0; i < nn; i++){
        b(i,0) = rhs;
    }
}

void Problem::solveSystem()
{
    Solver S("inner_mptiluc");
    S.SetParameter("relative_tolerance", "1e-10");
    S.SetParameter("absolute_tolerance", "1e-13");
    double t = Timer();

    Sparse::Matrix &J = R.GetJacobian();
//    ofstream oo("MAT.txt");
//    unsigned N = R.GetLastIndex();
//    cout << "N = " << N << endl;


//    for(unsigned i = 0; i < N; i++){
//        for(unsigned j = 0; j < i; j++)
//            J[i][j] = J[j][i];
//    }

//    double nnz = 0.0;
//    for(unsigned i = 0; i < N; i++){
//        for(unsigned j = 0; j < N; j++){
//            oo << J[i][j] << " ";
//            if(fabs(J[i][j]) > 1e-10)
//                nnz += 1.0;
//        }
//        oo << endl;
//    }
//    oo.close();
//    printf("Average nnz per row: %lf\n", nnz/N);

    S.SetMatrix(J);
    times[T_PRECOND] += Timer() - t;
    Sparse::Vector sol;
    sol.SetInterval(aut.GetFirstIndex(), aut.GetLastIndex());
    for(unsigned i = 0; i < sol.Size(); i++){
        sol[i] = 1.;
        //printf("b[%d] = %e\n", i, R.GetResidual()[i]);
    }
    t = Timer();
    bool solved = S.Solve(R.GetResidual(), sol);
    times[T_SOLVE] += Timer() - t;
    if(!solved){
        cout << "Linear solver failed: " << S.GetReason() << endl;
        cout << "Residual: " << S.Residual() << endl;
        exit(1);
    }
    cout << "Linear solver iterations: " << S.Iterations() << endl;

    t = Timer();
    double Cnorm = 0.0;
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetMarker(mrkDirNode))
            continue;

        inode->Real(tagSol) -= sol[var.Index(inode->getAsNode())];
        Cnorm = max(Cnorm, fabs(inode->Real(tagSol)-inode->Real(tagSolEx)));
    }
    cout << "|err|_C = " << Cnorm << endl;
    times[T_UPDATE] += Timer() - t;
}

void Problem::saveSolution(string path)
{
    double t = Timer();
    m.Save(path);
    times[T_IO] += Timer() - t;
}


int main(int argc, char *argv[])
{
    if(argc != 2){
        cout << "Usage: 2d_diffusion_vem <mesh_file>" << endl;
        return 1;
    }

    Problem P(argv[1]);
    P.initProblem();
    P.assembleGlobalSystem();
    P.solveSystem();
    P.saveSolution("res.vtk");

    return 0;
}
