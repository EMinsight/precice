#ifndef PRECICE_NO_MPI

#include "testing/Testing.hpp"

#include <precice/precice.hpp>
#include <vector>

BOOST_AUTO_TEST_SUITE(Integration)
BOOST_AUTO_TEST_SUITE(Serial)
BOOST_AUTO_TEST_SUITE(Lifecycle)
BOOST_AUTO_TEST_SUITE(Reconstruction)
PRECICE_TEST_SETUP("SolverOne"_on(1_rank), "SolverTwo"_on(1_rank))
BOOST_AUTO_TEST_CASE(ImplicitFinalize)
{
  PRECICE_TEST();
  for (auto n : {1, 2, 3})
    BOOST_TEST_CONTEXT("construction #" << n)
    {
      precice::Participant interface(context.name, context.config(), context.rank, context.size, context.comm());

      if (context.isNamed("SolverOne")) {
        auto   meshName = "MeshOne";
        double coords[] = {0.1, 1.2, 2.3};
        auto   vertexid = interface.setMeshVertex(meshName, coords);

        auto   dataName = "DataOne";
        double data[]   = {3.4, 4.5, 5.6};
        interface.writeData(meshName, dataName, {&vertexid, 1}, data);
      } else {
        auto   meshName = "MeshTwo";
        double coords[] = {0.12, 1.21, 2.2};
        auto   vertexid = interface.setMeshVertex(meshName, coords);

        auto   dataName = "DataTwo";
        double data[]   = {7.8};
        interface.writeData(meshName, dataName, {&vertexid, 1}, data);
      }
      interface.initialize();
      BOOST_TEST(interface.isCouplingOngoing());
    }
}

BOOST_AUTO_TEST_SUITE_END() // Reconstruction
BOOST_AUTO_TEST_SUITE_END() // Lifecycle
BOOST_AUTO_TEST_SUITE_END() // Serial
BOOST_AUTO_TEST_SUITE_END() // Integration

#endif // PRECICE_NO_MPI
