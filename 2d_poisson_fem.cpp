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

typedef struct{
    Sparse::Matrix A;
    Sparse::Vector b;
} LinearSystem;

const string tagNameTensor = "DIFFUSION_TENSOR";
const string tagNameBC     = "BOUNDARY_CONDITION";
const string tagNameRHS    = "RHS";
const string tagNameDir    = "DIRICHLET_NODE";
const string tagNameSol    = "SOLUTION";
const string tagNameSolEx  = "SOLUTION_EXACT";

const double Dxx = 1e2;
const double Dyy = 1e0;
const double Dxy = 0e0;

double exactSolution(double *x)
{
    return x[0]*x[0];
}

double exactSolutionRHS(double *x)
{
    return -2*Dxx;
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

    MarkerType mrkDirNode;  // Dirichlet node marker

    LinearSystem linSys;

    unsigned numDirNodes;
    unsigned size;        // size of resulting system = #nodes-#Dir.nodes

public:
    Problem(string meshName);
    ~Problem();
    void initProblem(); // create tags and set parameters
    void assembleGlobalSystem(); // assemble global linear system
    rMatrix computeStiffMatrix(Cell &);
    rMatrix integrateRHS(Cell &);
    void solveSystem();
    void saveSolution(string path); // save mesh with solution
};

Problem::Problem(string meshName)
{
    m.Load(meshName);
    cout << "Number of cells: " << m.NumberOfCells() << endl;
    cout << "Number of faces: " << m.NumberOfFaces() << endl;
    cout << "Number of edges: " << m.NumberOfEdges() << endl;
    cout << "Number of nodes: " << m.NumberOfNodes() << endl;
    m.AssignGlobalID(NODE);
}

Problem::~Problem()
{
}

void Problem::initProblem()
{
    tagD     = m.CreateTag(tagNameTensor, DATA_REAL, CELL, NONE, 3);
    tagBC    = m.CreateTag(tagNameBC,     DATA_REAL, NODE, NODE, 1);
    tagSol   = m.CreateTag(tagNameSol,    DATA_REAL, NODE, NONE, 1);
    tagSolEx = m.CreateTag(tagNameSolEx,  DATA_REAL, NODE, NONE, 1);
    tagRHS   = m.CreateTag(tagNameRHS,    DATA_REAL, NODE, NONE, 1);

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
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetStatus() == Element::Ghost)
            continue;
        Node node = inode->getAsNode();
        double x[2];
        node.Barycenter(x);

        node.Real(tagRHS) = exactSolutionRHS(x);
        node.Real(tagSolEx) = exactSolution(x);

        if(!node.Boundary())
            continue;

        node.SetMarker(mrkDirNode);
        numDirNodes++;
        node.Real(tagBC)  = exactSolution(x);
        node.Real(tagSol) = exactSolution(x);
    }
    cout << "Number of Dirichlet nodes: " << numDirNodes << endl;
}

void Problem::assembleGlobalSystem()
{

    Sparse::Matrix &A = linSys.A;
    Sparse::Vector &b = linSys.b;
    size = static_cast<unsigned>(m.NumberOfNodes())+1;
    A.SetInterval(0, size);
    b.SetInterval(0, size);
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;
        Cell cell = icell->getAsCell();

        ElementArray<Node> nodes = icell->getNodes();
        rMatrix stiffMatrix = computeStiffMatrix(cell);

//        cout << "stiffness matrix for cell " << cell.LocalID() << ":" << endl;
//        stiffMatrix.Print();
//        cout << endl << endl;

        rMatrix bRHS = integrateRHS(cell);
        bRHS *= 1;

        unsigned ind0 = static_cast<unsigned>(nodes[0].LocalID());
        unsigned ind1 = static_cast<unsigned>(nodes[1].LocalID());
        unsigned ind2 = static_cast<unsigned>(nodes[2].LocalID());
        if(nodes[0].GetMarker(mrkDirNode)){
            // There's no row corresponding to nodes[0]
            double bcVal = nodes[0].Real(tagBC);
            if(!nodes[1].GetMarker(mrkDirNode))
                b[ind1] -= bcVal * stiffMatrix(1,0);
            if(!nodes[2].GetMarker(mrkDirNode))
                b[ind2] -= bcVal * stiffMatrix(2,0);
        }
        else{
            A[ind0][ind0] += stiffMatrix(0,0);
            A[ind0][ind1] += stiffMatrix(1,0);
            A[ind0][ind2] += stiffMatrix(2,0);
            b[ind0] += bRHS(0,0);
        }

        if(nodes[1].GetMarker(mrkDirNode)){
            // Dirichlet node
            double bcVal = nodes[1].Real(tagBC);
            if(!nodes[0].GetMarker(mrkDirNode))
                b[ind0] -= bcVal * stiffMatrix(0,1);
            if(!nodes[2].GetMarker(mrkDirNode))
                b[ind2] -= bcVal * stiffMatrix(2,1);
        }
        else{
            A[ind1][ind0] += stiffMatrix(0,1);
            A[ind1][ind1] += stiffMatrix(1,1);
            A[ind1][ind2] += stiffMatrix(2,1);
            b[ind1] += bRHS(1,0);
        }

        if(nodes[2].GetMarker(mrkDirNode)){
            // Dirichlet node
            double bcVal = nodes[2].Real(tagBC);
            if(!nodes[1].GetMarker(mrkDirNode))
                b[ind1] -= bcVal * stiffMatrix(1,2);
            if(!nodes[0].GetMarker(mrkDirNode))
                b[ind0] -= bcVal * stiffMatrix(0,2);
        }
        else{
            A[ind2][ind0] += stiffMatrix(0,2);
            A[ind2][ind1] += stiffMatrix(1,2);
            A[ind2][ind2] += stiffMatrix(2,2);
            b[ind2] += bRHS(2,0);
        }
    }
}

rMatrix Problem::computeStiffMatrix(Cell &cell)
{
    ElementArray<Node> nodes = cell.getNodes();

    double x0[2], x1[2], x2[2];
    nodes[0].Barycenter(x0);
    nodes[1].Barycenter(x1);
    nodes[2].Barycenter(x2);

    rMatrix Dk(2,2); // Diffusion tensor
    Dk(0,0) = cell.RealArray(tagD)[0];
    Dk(1,1) = cell.RealArray(tagD)[1];
    Dk(1,0) = cell.RealArray(tagD)[2];
    Dk(0,1) = cell.RealArray(tagD)[2];

    rMatrix Bk(2,2);
    Bk(0,0) = x1[0] - x0[0]; //x2 - x1;
    Bk(0,1) = x2[0] - x0[0]; //x3 - x1;
    Bk(1,0) = x1[1] - x0[1]; //y2 - y1;
    Bk(1,1) = x2[1] - x0[1]; //y3 - y1;

    rMatrix Ck = Bk.Invert() * Dk * Bk.Invert().Transpose();
    //Ck = Dk * Ck;

    double detBk = Bk(0,0)*Bk(1,1) - Bk(0,1)*Bk(1,0);

    rMatrix Kee(3,3), Knn(3,3), Ken(3,3);
    Kee.Zero();
    Knn.Zero();
    Ken.Zero();

    Kee(0,0) = Kee(1,1) =  1.;
    Kee(0,1) = Kee(1,0) = -1.;
    Knn(0,0) = Knn(2,2) =  1.;
    Knn(0,2) = Knn(2,0) = -1.;
    Ken(0,0) = Ken(1,2) =  1.;
    Ken(1,0) = Ken(0,2) = -1.;

    Kee *= 0.5;
    Knn *= 0.5;
    Ken *= 0.5;

    rMatrix M(3,3);
    M = Ck(0,0)*Kee + Ck(1,1)*Knn + Ck(0,1)*(Ken + Ken.Transpose());
    M *= fabs(detBk);

    return M;
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
    Solver S("inner_ilu2");
    S.SetMatrix(linSys.A);
    Sparse::Vector sol;
    cout << "size = " << size << endl;
    sol.SetInterval(0, size);
    bool solved = S.Solve(linSys.b, sol);
    if(!solved){
        cout << "Linear solver failed: " << S.GetReason() << endl;
        cout << "Residual: " << S.Residual() << endl;
        exit(1);
    }
    cout << "Linear solver iterations: " << S.Iterations() << endl;

    double Cnorm = 0.0;
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetMarker(mrkDirNode))
            continue;

        inode->Real(tagSol) = sol[static_cast<unsigned>(inode->GlobalID())];
        Cnorm = max(Cnorm, fabs(inode->Real(tagSol)-inode->Real(tagSolEx)));
    }
    cout << "|err|_C = " << Cnorm << endl;
}

void Problem::saveSolution(string path)
{
    m.Save(path);
}

void setProblemParams(Mesh *m)
{
}


int main(int argc, char *argv[])
{
    if(argc != 2){
        cout << "Usage: 2d_poisson_fem <mesh_file>" << endl;
        return 1;
    }

    Problem P(argv[1]);
    P.initProblem();
    P.assembleGlobalSystem();
    P.solveSystem();
    P.saveSolution("res.vtk");

    return 0;
}
