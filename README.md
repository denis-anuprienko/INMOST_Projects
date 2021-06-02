# INMOST Projects
This repository contain implementation of numerical solution for different problems using INMOST (https://github.com/INMOST-DEV/INMOST) platform. This includes implementation of various discretizations methods for 2D and 3D elliptic problems, as well as testing of different coupling strategies for multiphysics problems. The code is intended to be as short and simple as possible.

Already implemented:
- ```2d_diffusion_fem.cpp``` - FEM for 2D diffusion (done for Dirichlet problem and linear triangular elements, following description from http://arturo.imati.cnr.it/~marini/didattica/Metodi-engl/Intro2FEM.pdf)
- ```2d_diffusion_fem_ad.cpp``` - version of ```2d_diffusion_fem.cpp``` based on INMOST's automatic differentiation (AD). Includes testing on a problem with rotated anisotropic diffusion tensor
- ```2d_diffusion_mfd.cpp``` - Mimetic finite difference for 2D diffusion (currently not working) in mixed form. Uses cell-centered pressure and face-centered flux unknowns
- ```2d_elasticity_fem.cpp``` - FEM for 2D linear elasticity (done for linear triangular elements and either Dirichlet BC or zero Neumann BC following https://link.springer.com/article/10.1007/s00607-002-1459-8)
- ```2d_dens_driven_flow.cpp``` - FVM for 2D density-driven flow. Uses two-point flux approximation (TPFA) for diffusion and flow in porous medium and simple upwind scheme for advection. Can be run on wide range of polygonal meshes, not only triangular. For solution of coupled problems either fully implicit or sequential implicit strategies can be used.

Future plans:
- FEM for 3D diffusion
- FVM (TPFA) for 3D diffusion equation 
- FEM for 3D linear elasticity
- VEM for 3D diffusion
- VEM for 3D linear elasticity
