#!/bin/bash
# Records a real OTF2 trace of an NAS Parallel Benchmark (NPB-MPI) with Score-P, as test data
# for the Trace Graph / timeline (see README.md). Submit: sbatch npb_scorep.sbatch
# Settings via environment, e.g.:  BENCH=lu CLASS=C NPROCS=128 sbatch -n 128 npb_scorep.sbatch
#
#SBATCH --job-name=npb-scorep
#SBATCH --ntasks=64
#SBATCH --time=00:20:00
#SBATCH --output=npb-scorep-%j.log

set -euo pipefail

BENCH=${BENCH:-bt}          # bt (square rank count: 64, 121, 256), lu, cg, mg, sp (square), ft, ep
CLASS=${CLASS:-C}           # C: about 1–3 min with 64 ranks; B is shorter, D much longer
NPROCS=${NPROCS:-${SLURM_NTASKS:-64}}
NPB_VERSION=${NPB_VERSION:-3.4.3}
WORK=${WORK:-$PWD/npb-scorep}
LAUNCH=${LAUNCH:-srun}      # srun (SLURM) or mpirun

# Score-P and MPI: adapt to the cluster (module names differ), or put scorep / mpif90 in PATH.
# module load mpi scorep
command -v scorep >/dev/null || { echo "scorep not in PATH (module load scorep?)"; exit 1; }
command -v mpif90 >/dev/null || { echo "mpif90 not in PATH (module load mpi?)"; exit 1; }

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "NPB$NPB_VERSION" ]; then
    curl -fLO "https://www.nas.nasa.gov/assets/nas/npb/NPB$NPB_VERSION.tar.gz"
    tar xzf "NPB$NPB_VERSION.tar.gz"
fi
cd "NPB$NPB_VERSION/NPB3.4-MPI"

# Compiled through Score-P: MPI calls only (--nocompiler), keeps the trace small; compute time is
# the time between the MPI calls.
cat > config/make.def <<MAKEDEF
MPIFC = scorep --mpp=mpi --nocompiler mpif90
FLINK = \$(MPIFC)
FFLAGS = -O2
FLINKFLAGS = -O2
MPICC = scorep --mpp=mpi --nocompiler mpicc
CLINK = \$(MPICC)
CFLAGS = -O2
CLINKFLAGS = -O2
CC = cc -g
BINDIR = ../bin
RAND = randi8
MAKEDEF
make "$BENCH" CLASS="$CLASS"

export SCOREP_ENABLE_TRACING=true
export SCOREP_ENABLE_PROFILING=false
export SCOREP_TOTAL_MEMORY=${SCOREP_TOTAL_MEMORY:-500M}   # per process; too small -> trace has gaps
export SCOREP_EXPERIMENT_DIRECTORY="$WORK/trace_${BENCH}.${CLASS}.${NPROCS}_$(date +%Y%m%d-%H%M%S)"

if [ "$LAUNCH" = srun ]; then
    srun -n "$NPROCS" "bin/$BENCH.$CLASS.x"
else
    mpirun -np "$NPROCS" "bin/$BENCH.$CLASS.x"
fi

echo
echo "Trace: $SCOREP_EXPERIMENT_DIRECTORY/traces.otf2"
du -sh "$SCOREP_EXPERIMENT_DIRECTORY"
