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
//    D is diffusion tensor, s.p.d. 3x3 matrix defined by 6 numbers Dxx, Dyy, Dzz, Dxy, Dxz, Dyz
//
//    The user should provide 3D mesh
//    (preferrably, a .vtk file which can be generated by Gmsh for example)
//    which is built for (0;1)x(0;1)x(0;1)
//
//    The code will then
//    - process mesh,
//    - init tags,
//    - assemble linear system,
//    - solve it with INMOST inner linear solver,
//    - save solution in a .vtk file.


using namespace INMOST;

enum
{
    T_ASSEMBLE = 0,
    T_SOLVE,
    T_PRECOND,
    T_IO,
    T_INIT,
    T_UPDATE
};

const std::string tagNameTensor = "DIFFUSION_TENSOR";
const std::string tagNameBC     = "BOUNDARY_CONDITION";
const std::string tagNameRHS    = "RHS";
const std::string tagNameSol    = "SOLUTION";
const std::string tagNameSolEx  = "SOLUTION_EXACT";

const int n_polys = 4;


// Corresponds to tensor
// [ 1  0 ]
// [ 0 10 ]
// rotated by M_PI/6
const double Dxx = 10;
const double Dyy = 2;
const double Dzz = 1;
const double Dxy = 0;
const double Dxz = 0;
const double Dyz = 0;

#ifndef M_PI
const double M_PI = 3.1415926535898;
#endif

double exactSolution(double *x)
{
    //return x[0];//sin(M_PI*x[0]) * sin(M_PI*x[1]);
    return sin(M_PI*x[0]) * sin(M_PI*x[1]) * sin(M_PI*x[2]);
}

double exactSolutionRHS(double *x)
{
    //return 0;//M_PI*M_PI * ((Dxx+Dyy) * exactSolution(x) - 2*Dxy*cos(M_PI*x[0])*cos(M_PI*x[1]));
    return M_PI*M_PI * ((Dxx+Dyy+Dzz) * exactSolution(x)
		    - 2*Dxy*cos(M_PI*x[0])*cos(M_PI*x[1])*sin(M_PI*x[2])
		    - 2*Dxz*cos(M_PI*x[0])*sin(M_PI*x[1])*cos(M_PI*x[2])
		    - 2*Dyz*sin(M_PI*x[0])*cos(M_PI*x[1])*cos(M_PI*x[2])
		    );
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

    Automatizator aut;    // Automatizator to handle all AD things
    Residual R;           // Residual to assemble
    dynamic_variable var; // Variable containing solution

    int rank; // for parallel runs

    int numDirNodes;

    double times[10];
    double ttt; // global timer

public:
    Problem(std::string meshName);
    ~Problem();
    void initProblem(); // create tags and set parameters
    void assembleGlobalSystem(); // assemble global linear system
    void assembleLocalSystem(Cell &, rMatrix &, rMatrix &);
    void solveSystem();
    void saveSolution(std::string path); // save mesh with solution
};

Problem::Problem(std::string meshName)
{
    ttt = Timer();
    for(int i = 0; i < 10; i++)
        times[i] = 0.;

    m.SetCommunicator(INMOST_MPI_COMM_WORLD);
    rank = m.GetProcessorRank();

    double t = Timer();

    if(m.isParallelFileFormat(meshName))
	    m.Load(meshName);
    else if(rank == 0)
    {
        m.Load(meshName);
        std::cout << "Number of cells: " << m.NumberOfCells() << std::endl;
        std::cout << "Number of faces: " << m.NumberOfFaces() << std::endl;
        std::cout << "Number of edges: " << m.NumberOfEdges() << std::endl;
        std::cout << "Number of nodes: " << m.NumberOfNodes() << std::endl;
    }

    if(m.GetProcessorsNumber() > 1)
    {
	Partitioner part(&m);
	part.SetMethod(Partitioner::INNER_KMEANS, Partitioner::Partition);
	part.Evaluate();
	m.Redistribute();
	m.AssignGlobalID(NODE);
	m.ExchangeGhost(1, NODE);
    }
    else
	m.AssignGlobalID(NODE);
    	
    {
	    Mesh::GeomParam param;
	    param[MEASURE] = CELL|FACE;
	    param[ORIENTATION] = FACE;
	    param[NORMAL] = FACE;
	    param[CENTROID] = CELL|FACE;
	    param[BARYCENTER] = CELL|FACE;
	    m.PrepareGeometricData(param);
    }

    times[T_IO] += Timer() - t;
}

Problem::~Problem()
{
	m.AggregateMax(times, 10);
	if(rank == 0)
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
}

void Problem::initProblem()
{
    double t = Timer();
    tagD     = m.CreateTag(tagNameTensor, DATA_REAL, CELL, NONE, 6);
    tagBC    = m.CreateTag(tagNameBC,     DATA_REAL, NODE, NODE, 1);
    tagSol   = m.CreateTag(tagNameSol,    DATA_REAL, NODE, NONE, 1);
    tagSolEx = m.CreateTag(tagNameSolEx,  DATA_REAL, NODE, NONE, 1);

    // Set diffusion tensor
    double D[6] = {Dxx,Dyy,Dzz,Dxy,Dxz,Dyz};
    for(Mesh::iteratorCell icell = m.BeginCell(); icell != m.EndCell(); icell++) if(icell->GetStatus() != Element::Ghost)
    {
	    for(int k = 0; k < 6; ++k)
		    icell->RealArray(tagD)[k] = D[k];
    }
    m.ExchangeData(tagD, CELL);

    // Set boundary conditions
    // Mark and count Dirichlet nodes
    // Compute RHS and exact solution
    mrkDirNode = m.CreateMarker();
    m.MarkBoundaryFaces(mrkDirNode);
    numDirNodes = 0;
    for(Mesh::iteratorNode inode = m.BeginNode(); inode != m.EndNode(); inode++) //if(inode->GetStatus() != Element::Ghost)
    {
        Node node = inode->getAsNode();
        double x[3];
        node.Barycenter(x);
        node.Real(tagSol) = node.Real(tagSolEx) = exactSolution(x);

	if(node.nbAdjElements(FACE, mrkDirNode))
	{
		node.SetMarker(mrkDirNode);
		numDirNodes++;
		node.Real(tagBC) = exactSolution(x);
	}
    }
    numDirNodes = m.Integrate(numDirNodes);
    if(rank == 0) std::cout << "Number of Dirichlet nodes: " << numDirNodes << std::endl;

    Automatizator::MakeCurrent(&aut);

    INMOST_DATA_ENUM_TYPE SolTagEntryIndex = aut.RegisterTag(tagSol, NODE, mrkDirNode, true);
    var = dynamic_variable(aut, SolTagEntryIndex);
    aut.EnumerateEntries();
    R = Residual("vem_diffusion", aut.GetFirstIndex(), aut.GetLastIndex());
    times[T_INIT] += Timer() - t;
}

void Problem::assembleGlobalSystem()
{
    double t = Timer();
    for(Mesh::iteratorCell icell = m.BeginCell(); icell != m.EndCell(); ++icell) //if(icell->GetStatus() != Element::Ghost)
    {
        Cell cell = icell->getAsCell();

        ElementArray<Node> nodes = icell->getNodes();
        rMatrix rhs, W;
        assembleLocalSystem(cell, W, rhs);

        int nnodes = nodes.size();

        for(int i = 0; i != nnodes; i++)
	{
            if(nodes[i]->GetMarker(mrkDirNode)) // boundary node
	    {
                double bcVal = nodes[i].Real(tagBC);
                for(int j = 0; j != nnodes; j++)
                    if(nodes[j].GetStatus() != Element::Ghost && !nodes[j].GetMarker(mrkDirNode))
                        R[var.Index(nodes[j])] += bcVal * W(j,i);
            }
            else if(nodes[i].GetStatus() != Element::Ghost) // Node with unknown
	    {
                for(int j = 0; j != nnodes; j++)
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
    ElementArray<Node> nodes = cell.getNodes();
    ElementArray<Face> faces = cell.getFaces();
    int nn = nodes.size(), nf = faces.size();
    double xc[3], diam = 0.0;
    cell.Centroid(xc);
    for(ElementArray<Node>::iterator it = nodes.begin(); it != nodes.end(); it++)
    {
	    Storage::real_array xi = it->Coords();
	    for(auto jt = nodes.begin(); jt != nodes.end(); jt++)
	    {
		    Storage::real_array xj = jt->Coords();
		    diam = std::max(diam, (xi[0]-xj[0])*(xi[0]-xj[0]) + (xi[1]-xj[1])*(xi[1]-xj[1]) + (xi[2]-xj[2])*(xi[2]-xj[2]));
	    }
    }
    diam = sqrt(diam);
    rMatrix D(nn, n_polys), B(n_polys, nn);
    B.Zero();
    D.Zero();
    for(int i = 0; i < nn; i++)
    {
	    D(i,0) = 1.0;
	    B(0,i) = 1.0/nn;
    }

    std::map<HandleType,int> gid_cnodes;
    for(int i = 0; i < nn; ++i) gid_cnodes[nodes[i].GlobalID()] = i;

    Storage::real_array K = cell.RealArray(tagD);
    for(int fid = 0; fid < nf; ++fid)
    {
	    Face f = faces[fid];
	    double area = f.Area();
	    double nrm[3], knrm[3];
	    f.OrientedUnitNormal(cell, nrm);
	    knrm[0] = K[0]*nrm[0] + K[3]*nrm[1] + K[4]*nrm[2];
	    knrm[1] = K[3]*nrm[0] + K[1]*nrm[1] + K[5]*nrm[2];
	    knrm[2] = K[4]*nrm[0] + K[5]*nrm[1] + K[2]*nrm[2];
	    ElementArray<Node> fnodes = f.getNodes();
	    int nfn = fnodes.size();
	    for(int k = 0; k < nfn; ++k) 
	    {
		    Node node = fnodes[k];
		    assert(gid_cnodes.find(node.GlobalID()) != gid_cnodes.end());
		    int i = gid_cnodes[node.GlobalID()];
		    assert(i < nn);
		    for(int j = 1; j < n_polys; ++j)
			    B(j,i) += 1.0/nfn * area / diam * knrm[j-1];
	    }
    }
    for(int vid = 0; vid < nn; ++vid)
    {
	    Storage::real_array xv = nodes[vid].Coords();
	    double monom[3] = { (xv[0]-xc[0])/diam, (xv[1]-xc[1])/diam, (xv[2]-xc[2])/diam };
	    for(int j = 1; j < n_polys; ++j)
		    D(vid,j) = monom[j-1];
    }
    int ierr = -1;
    rMatrix BDinv = (B*D).Invert(&ierr);
    if(ierr > 0) 
    {
	    std::cerr << "ierr = " << ierr << std::endl;
	    std::cerr << "B" << std::endl;
	    B.Print();
	    std::cerr << "D" << std::endl;
	    D.Print();
	    std::cerr << "B*D" << std::endl;
	    (B*D).Print();
    }
    rMatrix Proj = BDinv * B;
    rMatrix Se = rMatrix::Unit(nn) - D*Proj;
    rMatrix G = B*D;
    G(0,1,0,n_polys).Zero();
    W = Proj.Transpose() * G * Proj + Se.Transpose() * Se;
    double rhs = exactSolutionRHS(xc) * cell.Volume() / nn;
    b = rMatrix::Col(nn, rhs);
}

void Problem::solveSystem()
{
    Solver S("inner_ilu2", "test");
    S.SetParameter("relative_tolerance", "1e-10");
    S.SetParameter("absolute_tolerance", "1e-13");
    double t = Timer();

    S.SetMatrix(R.GetJacobian());
    times[T_PRECOND] += Timer() - t;
    Sparse::Vector sol;
    sol.SetInterval(aut.GetFirstIndex(), aut.GetLastIndex());
    std::fill(sol.Begin(), sol.End(), 0.0);
    t = Timer();
    bool solved = S.Solve(R.GetResidual(), sol);
    times[T_SOLVE] += Timer() - t;
    if(!solved)
    {
        std::cout << "Linear solver failed: " << S.GetReason() << std::endl;
        std::cout << "Residual: " << S.Residual() << std::endl;
        return;
    }
    if(rank == 0) std::cout << "Linear solver iterations: " << S.Iterations() << std::endl;

    t = Timer();
    double Cnorm = 0.0;
    for(Mesh::iteratorNode inode = m.BeginNode(); inode != m.EndNode(); inode++) if(inode->GetStatus() != Element::Ghost && !inode->GetMarker(mrkDirNode))
    {
        inode->Real(tagSol) -= sol[var.Index(inode->self())];
        Cnorm = std::max(Cnorm, fabs(inode->Real(tagSol)-inode->Real(tagSolEx)));
    }
    Cnorm = m.AggregateMax(Cnorm);
    if(rank == 0) std::cout << "|err|_C = " << Cnorm << std::endl;
    times[T_UPDATE] += Timer() - t;
}

void Problem::saveSolution(std::string prefix)
{
    double t = Timer();
    std::string extension;
    if(m.GetProcessorsNumber() > 1)
	    extension = ".pvtk";
    else
	    extension = ".vtk";
    m.Save(prefix + extension);
    times[T_IO] += Timer() - t;
}


int main(int argc, char *argv[])
{
    if(argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " <mesh_file>" << std::endl;
        return 1;
    }
    
    Solver::Initialize(&argc, &argv, "database.xml");
    Mesh::Initialize(&argc, &argv);
    Partitioner::Initialize(&argc, &argv);

    Problem* P = new Problem(argv[1]);
    P->initProblem();
    P->assembleGlobalSystem();
    P->solveSystem();
    P->saveSolution("res");

    delete P;

    Partitioner::Finalize();
    Solver::Finalize();
    Mesh::Finalize();

    return 0;
}