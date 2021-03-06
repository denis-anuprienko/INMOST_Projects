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
const string tagNameFlux   = "FLUX";


// Corresponds to tensor
// [ 1  0 ]
// [ 0 10 ]
// rotated by M_PI/6
const double Dxx = 1.0;//3.25;
const double Dyy = 10.0;//-0.433013;
const double Dxy = 0.0;//0.25;

const double M_PI = 3.1415926535898;

double exactSolution(double *x)
{
    return x[0];//sin(M_PI*x[0]) * sin(M_PI*x[1]);
}

double exactFlux(Face &f)
{
    double x[2], n[2];
    f.Barycenter(x);
    f.UnitNormal(n);
    double flux[2] = {-1., 0.};
    return flux[0]*n[0] + flux[1]*n[1];
}

double exactSolutionRHS(double *x)
{
    return 0.0;//M_PI*M_PI * ((Dxx+Dyy) * exactSolution(x) - 2*Dxy*cos(M_PI*x[0])*cos(M_PI*x[1]));
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
    Tag tagRHS;   // RHS function f
    Tag tagFlux;  // Flux

    MarkerType mrkDirNode;  // Dirichlet node marker

    Automatizator aut;     // Automatizator to handle all AD things
    Residual R;            // Residual to assemble
    dynamic_variable varP; // Variable containing solution
    dynamic_variable varU; // Variable containing flux

    unsigned numDirNodes;

    double times[10];
    double ttt; // global timer

public:
    Problem(string meshName);
    ~Problem();
    void initProblem(); // create tags and set parameters
    void assembleGlobalSystem(); // assemble global linear system
    void assembleLocalSystem(Cell &, rMatrix &);
    rMatrix integrateRHS(Cell &);
    void solveSystem();
    void saveSolution(string path); // save mesh with solution
};

Problem::Problem(string meshName)
{
    ttt = Timer();
    for(int i = 0; i < 10; i++)
        times[i] = 0.;

    double t = Timer();
    m.Load(meshName);
    cout << "Number of cells: " << m.NumberOfCells() << endl;
    cout << "Number of faces: " << m.NumberOfFaces() << endl;
    cout << "Number of edges: " << m.NumberOfEdges() << endl;
    cout << "Number of nodes: " << m.NumberOfNodes() << endl;
    m.AssignGlobalID(NODE);
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
    // Follow mimetic discretization framework
    // Pressure is defined at cells (C_h space) and at faces (Lambda_h space)
    // Flux     is defined at faces (F_h space)
    tagD     = m.CreateTag(tagNameTensor, DATA_REAL, CELL, NONE, 3);
    tagBC    = m.CreateTag(tagNameBC,     DATA_REAL, NODE, NODE, 1);
    tagSol   = m.CreateTag(tagNameSol,    DATA_REAL, CELL, NONE, 1);
    tagSolEx = m.CreateTag(tagNameSolEx,  DATA_REAL, CELL, NONE, 1);
    tagRHS   = m.CreateTag(tagNameRHS,    DATA_REAL, CELL, NONE, 1);
    tagFlux  = m.CreateTag(tagNameFlux,   DATA_REAL, FACE, NONE, 1);

    Automatizator::MakeCurrent(&aut);

    INMOST_DATA_ENUM_TYPE indP = 0, indU = 0;
    indP = aut.RegisterTag(tagSol, CELL);
    indU = aut.RegisterTag(tagFlux, FACE);
    varP = dynamic_variable(aut, indP);
    varU = dynamic_variable(aut, indU);
    aut.EnumerateEntries();
    R = Residual("mfd_diffusion", aut.GetFirstIndex(), aut.GetLastIndex());

    // Set diffusion tensor,
    // also check that all cells are triangles
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;

        icell->RealArray(tagD)[0] = Dxx; // Dxx
        icell->RealArray(tagD)[1] = Dyy; // Dyy
        icell->RealArray(tagD)[2] = Dxy; // Dxy

        double x[2];
        icell->Barycenter(x);
        icell->Real(tagSolEx) = exactSolution(x);
    }
    m.ExchangeData(tagD, CELL);

    // Set boundary conditions
    // Compute RHS and exact solution

    times[T_INIT] += Timer() - t;
}

void Problem::assembleGlobalSystem()
{
    double t = Timer();
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;
        Cell cell = icell->getAsCell();
        auto faces = cell.getFaces();
        unsigned nf = static_cast<unsigned>(faces.size());

        // nf x nf matrix defining flux inner product
        rMatrix MF;
        assembleLocalSystem(cell, MF);
//        MF.Zero();
//        for(unsigned i = 0; i < nf; i++)
//            MF(i,i) = cell.Volume();

        // Equations for flux: div_h u_h = 0 - assigned to cells
        int x = 0;
        for(auto f = faces.begin(); f != faces.end(); f++){
            double a = cell == f->FrontCell() ? -1. : 1.;
            a *= f->Area() / cell.Volume();
            R[varP.Index(cell)] += a * varU(f->getAsFace());
            x++;
        }
//        if(x != 4 || nf != 4){
//            cout << "x = " << x << endl;
//        }
//        double xP[2];
//        cell.Barycenter(xP);
//        R[varP.Index(cell)] = varP(cell) - exactSolution(xP);

        // Equations for pressure ~grad_h * [p Lambda] = 0 - assigned to faces
        Matrix<variable> res(nf,1);
        bool bnd = false;
        for(unsigned i = 0; i < nf; i++){
            Face f = faces[i];
            if(f.Boundary())
                bnd = true;
            double a = (cell == f->FrontCell() ? -1. : 1.);
            a *= f.Area();// / cell.Volume();
            double lam = 0.0;
            if(f.Boundary()){
                double x[2];
                f.Barycenter(x);
                lam = exactSolution(x);
                //cout << "lam = " << lam << endl;
            }
            res(i,0) = a * (varP(cell) - lam);
        }
        //if(!bnd)
        //    res = -MF.Invert() * res;
        //else
        //    res *= cell.Volume();

//        res.Print();
//        exit(1);

        // res contains action of local derived
        // gradient operator
        // on faces

//        for(unsigned i = 0; i < nf; i++){
//            Face f = faces[i];
//            R[varU.Index(f)] += res(i,0);
//            //R[varP.Index(f)] += varP(f) - 0.0;
//            R[varU.Index(f)] += varU(f);
//        }






        // NEW formulartion
        Matrix<variable> uc(nf,1);
        for(unsigned i = 0; i < nf; i++){
            Face f = faces[i];
            uc(i,0) = varU(f);
        }
//        res.Print();
//        exit(1);
        uc = MF * uc - res;
        //uc = uc - res;
        for(unsigned i = 0; i < nf; i++){
            Face f = faces[i];
            R[varU.Index(f)] += uc(i,0);
        }
    }

    for(auto iface = m.BeginFace(); iface != m.EndFace(); iface++){
        Face f = iface->getAsFace();
        //R[varU.Index(f)] += varU(f);// - exactFlux(f);
    }
    times[T_ASSEMBLE] += Timer() - t;
}

void Problem::assembleLocalSystem(Cell &cell, rMatrix &MF)
{
    auto faces = cell.getFaces();
    unsigned nf = static_cast<unsigned>(faces.size());

    double xP[2];
    cell.Barycenter(xP);

    rMatrix D(2,2); // Diffusion tensor
    D(0,0) = cell.RealArray(tagD)[0];
    D(1,1) = cell.RealArray(tagD)[1];
    D(1,0) = cell.RealArray(tagD)[2];
    D(0,1) = cell.RealArray(tagD)[2];


    rMatrix MP(nf,nf);
    rMatrix NP(nf,2);
    rMatrix RP(nf,2);
    // G   * [pc lam] = MF^(-1) * MAT
    // axb   (nf+1)x1   nfxnf    nfx1
    double xf[2], n[2];
    for(unsigned i = 0; i < nf; i++){
        faces[i].Barycenter(xf);
        faces[i].UnitNormal(n);
        NP(i,0) = n[0];
        NP(i,1) = n[1];

        double a = (cell == faces[i].FrontCell()) ? -1. : 1.;
        a *= faces[i].Area();// / cell.Volume();
        RP(i,0) = a * (xf[0] - xP[0]);
        RP(i,1) = a * (xf[1] - xP[1]);
    }
    NP = NP * D;
    //NP = D * NP;

    //rMatrix test = NP.Transpose() * RP - cell.Volume() * D;
    rMatrix test = RP.Transpose()*NP - cell.Volume() * D;
    double diff;
    if((diff = test.FrobeniusNorm()) > 1e-3){
        cout << "Bad test: diff = " << diff << endl;
        exit(1);
    }
//    test.Print();
//    exit(1);

    rMatrix MP0(nf,nf), MP1(nf,nf), I(nf,nf);
//    I.Unit(nf,1.0);
    I.Zero();
    for(unsigned i = 0; i < nf; i++)
        I(i,i) = 1.0;
    //cout << "tr = " << I.Trace() << endl;
    //I.Print();
    //exit(1);

    //MP0 = RP * (1./cell.Volume() * D.Invert()) * RP.Transpose();
    MP0 = RP * (RP.Transpose()*NP).Invert() * RP.Transpose();

    //double gammaP = 2*(RP * D.Invert() * RP.Transpose()).Trace() / nf / cell.Volume();// * 2.;
    double gammaP = MP0.Trace() / nf;
    MP1 = gammaP * (I - NP * (NP.Transpose()*NP).Invert() * NP.Transpose());
    MP = MP0 + MP1;

    MF = MP;
//    MF.Print();
//    cout << endl;
}


rMatrix Problem::integrateRHS(Cell &cell)
{
    rMatrix res(3,1);

    ElementArray<Node> nodes = cell.getNodes();

    double x0[2], x1[2], x2[2];
    nodes[0].Barycenter(x0);
    nodes[1].Barycenter(x1);
    nodes[2].Barycenter(x2);

    rMatrix Bk(2,2);
    Bk(0,0) = x1[0] - x0[0]; //x2 - x1;
    Bk(0,1) = x2[0] - x0[0]; //x3 - x1;
    Bk(1,0) = x1[1] - x0[1]; //y2 - y1;
    Bk(1,1) = x2[1] - x0[1]; //y3 - y1;

    rMatrix Ck = Bk.Invert() * Bk.Invert().Transpose();

    double detBk = Bk(0,0)*Bk(1,1) - Bk(0,1)*Bk(1,0);

    res.Zero();
    res(0,0) += exactSolutionRHS(x0) + exactSolutionRHS(x1) + exactSolutionRHS(x2);
    res(1,0) = res(0,0);
    res(2,0) = res(0,0);

    return res * fabs(detBk) / 18.;
}

void Problem::solveSystem()
{
    Solver S("inner_mptiluc");
    S.SetParameter("maximum_iterations", "10000");
    double t = Timer();

    Sparse::Matrix &J = R.GetJacobian();
    ofstream oo("MAT.txt");
    unsigned N = R.GetLastIndex();
    cout << "N = " << N << endl;


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
    //R.GetResidual().Save("J.mtx");
    times[T_PRECOND] += Timer() - t;
    Sparse::Vector sol;
    sol.SetInterval(aut.GetFirstIndex(), aut.GetLastIndex());
    for(unsigned i = 0; i < sol.Size(); i++){
        sol[i] = i;//rand();
    }
    printf("System size is %d\n", (sol.Size()));
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
    double CnormP = 0.0, CnormQ = 0.0;
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        Cell c = icell->getAsCell();
        c.Real(tagSol) -= sol[varP.Index(c)];
        CnormP = max(CnormP, fabs(c.Real(tagSol)-c.Real(tagSolEx)));
    }
    for(auto iface = m.BeginFace(); iface != m.EndFace(); iface++){
        Face f = iface->getAsFace();
        f.Real(tagFlux) -= sol[varU.Index(f)];
        CnormQ = max(CnormQ, fabs(f.Real(tagFlux)-exactFlux(f)));
        //printf("face %d: f = %e\n", f.LocalID(), f.Real(tagFlux));
    }
    cout << "|errP|_C = " << CnormP << endl;
    cout << "|errQ|_C = " << CnormQ << endl;
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
        cout << "Usage: 2d_diffusion_mfd <mesh_file>" << endl;
        return 1;
    }

    Problem P(argv[1]);
    P.initProblem();
    P.assembleGlobalSystem();
    P.solveSystem();
    P.saveSolution("res.vtk");

    return 0;
}
