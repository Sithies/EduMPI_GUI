# Echte Traces für die Trace-Ansichten (Score-P + NAS Parallel Benchmarks)

Die synthetischen Traces aus `tools/big-trace` testen Größe und Ladezeit, aber nicht, ob die
Befunde der Trace-Graph-Ansicht bei echten Programmen stimmen und verständlich sind. Dafür ein
kurzer Lauf eines bekannten MPI-Benchmarks mit Score-P auf dem Cluster.

## Was wird gebraucht
- Score-P (≥ 7) und ein MPI mit Fortran-Compiler (`mpif90`), z. B. per `module load`
- Internetzugang zum Herunterladen der NPB-Quellen (oder `NPB3.4.3.tar.gz` vorher ablegen)
- 64–256 Ranks für 5–20 Minuten (Übersetzen + Lauf)

## Starten
```sh
# Standard: BT, Klasse C, 64 Ranks
sbatch npb_scorep.sbatch

# LU mit 128 Ranks (Wellenfront: viele Late Sender, gut für die Wartezeit-Befunde)
BENCH=lu CLASS=C NPROCS=128 sbatch -n 128 npb_scorep.sbatch

# ohne SLURM
LAUNCH=mpirun NPROCS=64 ./npb_scorep.sbatch
```
Die Ausgabe nennt am Ende den Ordner mit `traces.otf2`. Diesen ganzen Ordner (mit `traces/` und
`traces.def`) in der GUI über „Trace öffnen“ laden.

Hinweise:
- BT und SP brauchen eine Quadratzahl an Ranks (64, 121, 256), LU/CG/MG eine Zweierpotenz.
- Übersetzt wird mit `--nocompiler`: nur MPI-Aufrufe werden aufgezeichnet. So bleibt der Trace
  klein (Klasse C, 64 Ranks: meist einige 100 MB); Rechenzeit ist die Zeit zwischen MPI-Aufrufen.
- Meldet Score-P „buffer flush“ oder fehlen Teile, `SCOREP_TOTAL_MEMORY` erhöhen (z. B. `1G`).

## Welche Läufe sind interessant
| Lauf | Was man in der Graph-Ansicht sehen sollte |
|---|---|
| `bt`, 64 Ranks | 2D-Nachbarschaft (Gitter/Torus), gleichmäßige Last, wenig Warten |
| `lu`, 128 Ranks | Gitter, Wellenfront: Late Sender und Wartekette von Ecke zu Ecke |
| `cg`, 128 Ranks | Gruppen (Cluster), viele kleine Nachrichten, Collectives |
| `ft`, 64 Ranks | Alle-mit-allen (MPI_Alltoall), Collectives dominieren |
| beliebig, ein Knoten überbucht oder langsamer | Befunde „langsamer Knoten“, „Lastungleichgewicht“, Zusammenfassung rot |

## Was dabei zu prüfen ist
1. Stimmt der Satz oben über den Befunden mit dem Eindruck aus der Timeline überein?
2. Zeigt das automatische Layout die erwartete Struktur (Gitter bei BT/LU)?
3. Sind die als Ursache genannten Ranks (Late Sender, zuletzt an der Collective) plausibel?
4. Verstehen Studierende die Karten und die Hilfe („Was sehe ich hier?“) ohne Erklärung?
