// SPDX-License-Identifier: MIT

#include "NetsToChipConnections.h"

#include <Arduino.h>
#include <EEPROM.h>

// Compatibility for clangd - these are provided by Arduino.h at compile time
#ifndef malloc
extern void *malloc(size_t size);
#endif
#ifndef free
extern void free(void *ptr);
#endif
#ifndef strlen
extern size_t strlen(const char *str);
#endif
#ifndef strcpy
extern char *strcpy(char *dest, const char *src);
#endif

#include "Commands.h"
#include "Graphics.h"
#include "JumperlessDefines.h"
#include "MatrixState.h"
#include "NetManager.h"
#include "Peripherals.h"
#include "Probing.h"
//#include "SerialWrapper.h"

//#define Serial SerialWrap

// Compile-time debug flags - set to 1 to enable, 0 to disable
#ifndef DEBUG_NTCC1_ENABLED
#define DEBUG_NTCC1_ENABLED 0  // Basic path routing debug
#endif

#ifndef DEBUG_NTCC2_ENABLED
#define DEBUG_NTCC2_ENABLED 0  // Detailed routing and alt paths
#endif

#ifndef DEBUG_NTCC3_ENABLED
#define DEBUG_NTCC3_ENABLED 0  // Path conflicts and overlaps
#endif

#ifndef DEBUG_NTCC5_ENABLED
#define DEBUG_NTCC5_ENABLED 0  // Bridge-to-path conversion debug
#endif

#ifndef DEBUG_NTCC6_ENABLED
#define DEBUG_NTCC6_ENABLED 0  // Transaction state and conflict validation
#endif

// Debug macros - completely removed at compile time when disabled
#if DEBUG_NTCC1_ENABLED
  #define DEBUG_NTCC1_PRINT(x) do { if(debugNTCC) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC1_PRINTLN(x) do { if(debugNTCC) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC1_PRINTF(fmt, ...) do { if(debugNTCC) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC1_PRINT(x)
  #define DEBUG_NTCC1_PRINTLN(x)
  #define DEBUG_NTCC1_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC2_ENABLED
  #define DEBUG_NTCC2_PRINT(x) do { if(debugNTCC2) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC2_PRINTLN(x) do { if(debugNTCC2) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC2_PRINTF(fmt, ...) do { if(debugNTCC2) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC2_PRINT(x)
  #define DEBUG_NTCC2_PRINTLN(x)
  #define DEBUG_NTCC2_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC3_ENABLED
  #define DEBUG_NTCC3_PRINT(x) do { if(debugNTCC3) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC3_PRINTLN(x) do { if(debugNTCC3) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC3_PRINTF(fmt, ...) do { if(debugNTCC3) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC3_PRINT(x)
  #define DEBUG_NTCC3_PRINTLN(x)
  #define DEBUG_NTCC3_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC5_ENABLED
  #define DEBUG_NTCC5_PRINT(x) do { if(debugNTCC5) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC5_PRINTLN(x) do { if(debugNTCC5) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC5_PRINTF(fmt, ...) do { if(debugNTCC5) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC5_PRINT(x)
  #define DEBUG_NTCC5_PRINTLN(x)
  #define DEBUG_NTCC5_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC6_ENABLED
  #define DEBUG_NTCC6_PRINT(x) do { if(debugNTCC6) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC6_PRINTLN(x) do { if(debugNTCC6) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC6_PRINTF(fmt, ...) do { if(debugNTCC6) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC6_PRINT(x)
  #define DEBUG_NTCC6_PRINTLN(x)
  #define DEBUG_NTCC6_PRINTF(fmt, ...)
#endif

// Convenience macro for any NTCC debug output
#if DEBUG_NTCC1_ENABLED || DEBUG_NTCC2_ENABLED || DEBUG_NTCC3_ENABLED || DEBUG_NTCC5_ENABLED || DEBUG_NTCC6_ENABLED
  #define DEBUG_NTCC_ANY_ENABLED 1
#else
  #define DEBUG_NTCC_ANY_ENABLED 0
#endif

/*
 * USAGE EXAMPLES FOR CONVERTING DEBUG CODE:
 * 
 * OLD: if (debugNTCC2) { Serial.print("Value: "); Serial.println(value); }
 * NEW: DEBUG_NTCC2_PRINT("Value: "); DEBUG_NTCC2_PRINTLN(value);
 * 
 * OLD: if (debugNTCC6) { Serial.printf("Chip %c at %d,%d\n", chip, x, y); }
 * NEW: DEBUG_NTCC6_PRINTF("Chip %c at %d,%d\n", chip, x, y);
 * 
 * For expensive validation functions:
 * OLD: someExpensiveValidationFunction();
 * NEW: #if DEBUG_NTCC6_ENABLED
 *      someExpensiveValidationFunction();
 *      #endif
 * 
 * Debug level meanings:
 * NTCC1 - Basic path routing (replaces debugNTCC)
 * NTCC2 - Detailed routing and alt paths (replaces debugNTCC2) 
 * NTCC3 - Path conflicts and overlaps (replaces debugNTCC3)
 * NTCC5 - Bridge-to-path conversion (replaces debugNTCC5)
 * NTCC6 - Transaction state validation (replaces debugNTCC6)
 */

// don't try to understand this, it's still a mess
bool debugNTCC5 = false;
int startEndChip[2] = {-1, -1};
int bothNodes[2] = {-1, -1};
int endChip = -1;
int chipCandidates[2][4] = {
    {-1, -1, -1, -1},
    {-1, -1, -1,
     -1}}; // nano and sf nodes have multiple possible chips they could be
// connected to, so we need to store them all and check them all

int chipsLeastToMostCrowded[12] = {
    0, 1, 2, 3, 4,  5,
    6, 7, 8, 9, 10, 11}; // this will be sorted from most to least crowded, and
// will be used to determine which chip to use for
// each node
int sfChipsLeastToMostCrowded[4] = {
    8, 9, 10, 11}; // this will be sorted from most to least crowded, and will
// be used to determine which chip to use for each node

int numberOfUniqueNets = 0;
int numberOfNets = 0;
volatile int numberOfPaths = 0;

int pathsWithCandidates[MAX_BRIDGES] = {0};
int pathsWithCandidatesIndex = 0;

int numberOfUnconnectablePaths = 0;
int unconnectablePaths[10][2] = {
    {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
    {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
};
int newBridges[MAX_NETS][MAX_DUPLICATE][2] = {0};
unsigned long timeToSort = 0;

bool debugNTCC = 0; // EEPROM.read(DEBUG_NETTOCHIPCONNECTIONSADDRESS);

bool debugNTCC2 = 0; // EEPROM.read(DEBUG_NETTOCHIPCONNECTIONSALTADDRESS);

bool debugNTCC3 = false;

bool debugNTCC4 = true; // Debug for forcing path updates

bool debugNTCC6 = false; // Debug for ijkl paths and direct connections

// State backup structures for transactional assignment
struct ChipStateBackup {
  int8_t xStatus[16];
  int8_t yStatus[8];
};

struct PathStateBackup {
  int net;
  int node1;
  int node2;
  int chip[4];
  int x[6];
  int y[6];
  bool altPathNeeded;
  bool sameChip;
  bool skip;
};




static ChipStateBackup chipBackup[12];
static PathStateBackup pathBackup;
static bool backupValid = false;

// Helper function to save current chip and path state
void saveRoutingState(int pathIndex) {
  // Save all chip states
  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 16; j++) {
      chipBackup[i].xStatus[j] = ch[i].xStatus[j];
    }
    for (int j = 0; j < 8; j++) {
      chipBackup[i].yStatus[j] = ch[i].yStatus[j];
    }
  }

  // Save path state
  pathBackup.net = path[pathIndex].net;
  pathBackup.node1 = path[pathIndex].node1;
  pathBackup.node2 = path[pathIndex].node2;
  pathBackup.altPathNeeded = path[pathIndex].altPathNeeded;
  pathBackup.sameChip = path[pathIndex].sameChip;
  pathBackup.skip = path[pathIndex].skip;

  for (int i = 0; i < 4; i++) {
    pathBackup.chip[i] = path[pathIndex].chip[i];
  }
  for (int i = 0; i < 6; i++) {
    pathBackup.x[i] = path[pathIndex].x[i];
    pathBackup.y[i] = path[pathIndex].y[i];
  }

  backupValid = true;

  DEBUG_NTCC6_PRINT("State saved for path[");
  DEBUG_NTCC6_PRINT(pathIndex);
  DEBUG_NTCC6_PRINTLN("]");
}

// Helper function to restore chip and path state from backup
void restoreRoutingState(int pathIndex) {
  if (!backupValid) {
    if (debugNTCC6) {
      Serial.println(
          "ERROR: Attempted to restore state but no backup available!");
    }
    return;
  }

  // Restore all chip states
  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 16; j++) {
      ch[i].xStatus[j] = chipBackup[i].xStatus[j];
    }
    for (int j = 0; j < 8; j++) {
      ch[i].yStatus[j] = chipBackup[i].yStatus[j];
    }
  }

  // Restore path state
  path[pathIndex].net = pathBackup.net;
  path[pathIndex].node1 = pathBackup.node1;
  path[pathIndex].node2 = pathBackup.node2;
  path[pathIndex].altPathNeeded = pathBackup.altPathNeeded;
  path[pathIndex].sameChip = pathBackup.sameChip;
  path[pathIndex].skip = pathBackup.skip;

  for (int i = 0; i < 4; i++) {
    path[pathIndex].chip[i] = pathBackup.chip[i];
  }
  for (int i = 0; i < 6; i++) {
    path[pathIndex].x[i] = pathBackup.x[i];
    path[pathIndex].y[i] = pathBackup.y[i];
  }

  DEBUG_NTCC6_PRINT("State restored for path[");
  DEBUG_NTCC6_PRINT(pathIndex);
  DEBUG_NTCC6_PRINTLN("]");
}

// Helper function to commit current state (clear backup)
void commitRoutingState(void) {
  backupValid = false;
  if (debugNTCC6) {
    Serial.println("State committed (backup cleared)");
  }
}

// Helper function to validate state consistency after transactions
void validateTransactionConsistency(void) {
#if DEBUG_NTCC6_ENABLED
  DEBUG_NTCC6_PRINTLN("\n=== TRANSACTION CONSISTENCY CHECK ===");

    // Check for paths that have assigned positions but skip flag is set
    for (int i = 0; i < numberOfPaths; i++) {
      if (path[i].skip) {
        bool hasAssignedPositions = false;
        for (int j = 0; j < 4; j++) {
          if (path[i].x[j] >= 0 || path[i].y[j] >= 0) {
            hasAssignedPositions = true;
            break;
          }
        }
        if (hasAssignedPositions) {
          Serial.print("INCONSISTENCY: Path[");
          Serial.print(i);
          Serial.println("] has skip=true but has assigned positions");
        }
      }
    }

    // Check for chip positions assigned to multiple nets
    for (int chip = 0; chip < 12; chip++) {
      for (int x = 0; x < 16; x++) {
        if (ch[chip].xStatus[x] > 0) {
          int pathCount = 0;
          for (int p = 0; p < numberOfPaths; p++) {
            if (path[p].skip)
              continue;
            for (int seg = 0; seg < 4; seg++) {
              if (path[p].chip[seg] == chip && path[p].x[seg] == x) {
                pathCount++;
              }
            }
          }
          if (pathCount == 0) {
            Serial.print("INCONSISTENCY: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" X[");
            Serial.print(x);
            Serial.print("] assigned to net ");
            Serial.print(ch[chip].xStatus[x]);
            Serial.println(" but no paths use it");
          }
        }
      }

      for (int y = 0; y < 8; y++) {
        if (ch[chip].yStatus[y] > 0) {
          int pathCount = 0;
          for (int p = 0; p < numberOfPaths; p++) {
            if (path[p].skip)
              continue;
            for (int seg = 0; seg < 4; seg++) {
              if (path[p].chip[seg] == chip && path[p].y[seg] == y) {
                pathCount++;
              }
            }
          }
          if (pathCount == 0) {
            Serial.print("INCONSISTENCY: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" Y[");
            Serial.print(y);
            Serial.print("] assigned to net ");
            Serial.print(ch[chip].yStatus[y]);
            Serial.println(" but no paths use it");
          }
        }
      }
    }

    DEBUG_NTCC6_PRINTLN("=== END CONSISTENCY CHECK ===\n");
#endif
}

// Helper function to track direct Y status assignments
void setChipYStatus(int chip, int y, int net, const char *location) {
  if (debugNTCC6 && ch[chip].yStatus[y] != -1 && ch[chip].yStatus[y] != net) {
    DEBUG_NTCC6_PRINT("DIRECT Y ASSIGNMENT CONFLICT: ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" Y[");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT("] occupied by net ");
    DEBUG_NTCC6_PRINT(ch[chip].yStatus[y]);
    DEBUG_NTCC6_PRINT(", overwriting with net ");
    DEBUG_NTCC6_PRINTLN(net);
  }
  ch[chip].yStatus[y] = net;
}

// Helper function to detect and report routing conflicts
void detectAndReportConflicts(void) {
#if DEBUG_NTCC6_ENABLED
  DEBUG_NTCC6_PRINTLN("\n=== CONFLICT DETECTION REPORT ===");

    // Check for X conflicts
    for (int chip = 0; chip < 12; chip++) {
      for (int x = 0; x < 16; x++) {
        if (ch[chip].xStatus[x] > 0) {
          // Count how many paths use this X position
          int pathCount = 0;
          int nets[MAX_NETS];
          int netCount = 0;

          for (int pathIdx = 0; pathIdx < numberOfPaths; pathIdx++) {
            if (path[pathIdx].skip)
              continue;

            for (int seg = 0; seg < 4; seg++) {
              if (path[pathIdx].chip[seg] == chip &&
                  path[pathIdx].x[seg] == x) {
                pathCount++;

                // Track unique nets
                bool netExists = false;
                for (int n = 0; n < netCount; n++) {
                  if (nets[n] == path[pathIdx].net) {
                    netExists = true;
                    break;
                  }
                }
                if (!netExists && netCount < MAX_NETS) {
                  nets[netCount++] = path[pathIdx].net;
                }
              }
            }
          }

          if (netCount > 1) {
            Serial.print("X CONFLICT: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" X[");
            Serial.print(x);
            Serial.print("] used by nets: ");
            for (int n = 0; n < netCount; n++) {
              Serial.print(nets[n]);
              if (n < netCount - 1)
                Serial.print(", ");
            }
            Serial.println();
          }
        }
      }
    }

    // Check for Y conflicts
    for (int chip = 0; chip < 12; chip++) {
      for (int y = 0; y < 8; y++) {
        if (ch[chip].yStatus[y] > 0) {
          // Count how many paths use this Y position
          int pathCount = 0;
          int nets[MAX_NETS];
          int netCount = 0;

          for (int pathIdx = 0; pathIdx < numberOfPaths; pathIdx++) {
            if (path[pathIdx].skip)
              continue;

            for (int seg = 0; seg < 4; seg++) {
              if (path[pathIdx].chip[seg] == chip &&
                  path[pathIdx].y[seg] == y) {
                pathCount++;

                // Track unique nets
                bool netExists = false;
                for (int n = 0; n < netCount; n++) {
                  if (nets[n] == path[pathIdx].net) {
                    netExists = true;
                    break;
                  }
                }
                if (!netExists && netCount < MAX_NETS) {
                  nets[netCount++] = path[pathIdx].net;
                }
              }
            }
          }

          if (netCount > 1) {
            Serial.print("Y CONFLICT: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" Y[");
            Serial.print(y);
            Serial.print("] used by nets: ");
            for (int n = 0; n < netCount; n++) {
              Serial.print(nets[n]);
              if (n < netCount - 1)
                Serial.print(", ");
            }
            Serial.println();
          }
        }
      }
    }

    DEBUG_NTCC6_PRINTLN("=== END CONFLICT DETECTION ===\n");
#endif
}

// Helper function to safely set chip X status with validation
bool setChipXStatus(int chip, int x, int net, const char *location) {
  // Bounds checking to prevent crashes
  if (chip < 0 || chip >= 12 || x < 0 || x >= 16 || net < 0 ||
      net >= MAX_NETS) {
    DEBUG_NTCC6_PRINT("ERROR: Invalid parameters in setChipXStatus - ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" chip=");
    DEBUG_NTCC6_PRINT(chip);
    DEBUG_NTCC6_PRINT(" x=");
    DEBUG_NTCC6_PRINT(x);
    DEBUG_NTCC6_PRINT(" net=");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Invalid parameters, assignment failed
  }

  // Check for overlap conflicts before assignment
  if (ch[chip].xStatus[x] != -1 && ch[chip].xStatus[x] != net) {
    DEBUG_NTCC6_PRINT("WARNING: X position conflict in ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" X[");
    DEBUG_NTCC6_PRINT(x);
    DEBUG_NTCC6_PRINT("] already occupied by net ");
    DEBUG_NTCC6_PRINT(ch[chip].xStatus[x]);
    DEBUG_NTCC6_PRINT(", trying to assign net ");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Assignment failed due to conflict
  }

  // Assign the X position
  ch[chip].xStatus[x] = net;

  DEBUG_NTCC6_PRINT("SUCCESS: ");
  DEBUG_NTCC6_PRINT(location);
  DEBUG_NTCC6_PRINT(" - Assigned X=");
  DEBUG_NTCC6_PRINT(x);
  DEBUG_NTCC6_PRINT(" on chip ");
  DEBUG_NTCC6_PRINT(chipNumToChar(chip));
  DEBUG_NTCC6_PRINT(" to net ");
  DEBUG_NTCC6_PRINTLN(net);

  return true; // Assignment successful
}

// Helper function to safely set chip Y status with validation (improved
// version)
bool setChipYStatusSafe(int chip, int y, int net, const char *location) {
  // Bounds checking to prevent crashes
  if (chip < 0 || chip >= 12 || y < 0 || y >= 8 || net < 0 || net >= MAX_NETS) {
    DEBUG_NTCC6_PRINT("ERROR: Invalid parameters in setChipYStatusSafe - ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" chip=");
    DEBUG_NTCC6_PRINT(chip);
    DEBUG_NTCC6_PRINT(" y=");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT(" net=");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Invalid parameters, assignment failed
  }

  // Check for overlap conflicts before assignment
  if (ch[chip].yStatus[y] != -1 && ch[chip].yStatus[y] != net) {
    DEBUG_NTCC6_PRINT("WARNING: Y position conflict in ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" Y[");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT("] already occupied by net ");
    DEBUG_NTCC6_PRINT(ch[chip].yStatus[y]);
    DEBUG_NTCC6_PRINT(", trying to assign net ");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Assignment failed due to conflict
  }

  // Assign the Y position
  ch[chip].yStatus[y] = net;

  DEBUG_NTCC6_PRINT("SUCCESS: ");
  DEBUG_NTCC6_PRINT(location);
  DEBUG_NTCC6_PRINT(" - Assigned Y=");
  DEBUG_NTCC6_PRINT(y);
  DEBUG_NTCC6_PRINT(" on chip ");
  DEBUG_NTCC6_PRINT(chipNumToChar(chip));
  DEBUG_NTCC6_PRINT(" to net ");
  DEBUG_NTCC6_PRINTLN(net);

  return true; // Assignment successful
}

// Helper function to safely set path X coordinates with validation
bool setPathX(int pathIndex, int xIndex, int xValue) {
  // Bounds checking to prevent crashes
  if (pathIndex < 0 || pathIndex >= MAX_BRIDGES || xIndex < 0 || xIndex >= 16) {
    return false; // Invalid parameters, assignment failed
  }

  // Allow special values (-2, -1) without conflict checking
  if (xValue < 0) {
    path[pathIndex].x[xIndex] = xValue;
    return true;
  }

  // Check for conflicts when overwriting existing real coordinates
  if (path[pathIndex].x[xIndex] >= 0 && path[pathIndex].x[xIndex] != xValue) {
    DEBUG_NTCC6_PRINT("WARNING: X coordinate conflict - path[");
    DEBUG_NTCC6_PRINT(pathIndex);
    DEBUG_NTCC6_PRINT("].x[");
    DEBUG_NTCC6_PRINT(xIndex);
    DEBUG_NTCC6_PRINT("] already set to ");
    DEBUG_NTCC6_PRINT(path[pathIndex].x[xIndex]);
    DEBUG_NTCC6_PRINT(", trying to assign ");
    DEBUG_NTCC6_PRINTLN(xValue);
    return false; // Conflict detected
  }

  // Assign the X coordinate
  path[pathIndex].x[xIndex] = xValue;
  return true; // Assignment successful
}

// Helper function to safely set path Y coordinates with validation
bool setPathY(int pathIndex, int yIndex, int yValue) {
  // Bounds checking to prevent crashes
  if (pathIndex < 0 || pathIndex >= MAX_BRIDGES || yIndex < 0 || yIndex >= 8) {
    return false; // Invalid parameters, assignment failed
  }

  // Allow special values (-2, -1) without conflict checking
  if (yValue < 0) {
    path[pathIndex].y[yIndex] = yValue;
    return true;
  }

  // Check for conflicts when overwriting existing real coordinates
  if (path[pathIndex].y[yIndex] >= 0 && path[pathIndex].y[yIndex] != yValue) {
    DEBUG_NTCC6_PRINT("WARNING: Y coordinate conflict - path[");
    DEBUG_NTCC6_PRINT(pathIndex);
    DEBUG_NTCC6_PRINT("].y[");
    DEBUG_NTCC6_PRINT(yIndex);
    DEBUG_NTCC6_PRINT("] already set to ");
    DEBUG_NTCC6_PRINT(path[pathIndex].y[yIndex]);
    DEBUG_NTCC6_PRINT(", trying to assign ");
    DEBUG_NTCC6_PRINTLN(yValue);
    return false; // Conflict detected
  }

  // Assign the Y coordinate
  path[pathIndex].y[yIndex] = yValue;
  return true; // Assignment successful
}

int pathIndex = 0;

// int powerDuplicates = 2;
// int dacDuplicates = 0;
// int pathDuplicates = 2;
// int powerPriority = 1;
// int dacPriority = 1;

// Y position limits to prevent high-priority nets from using all positions
int yPositionLimits[MAX_NETS] = {0}; // 0 = no limit
int yPositionUsage[MAX_NETS] = {0};  // Track current usage per net

// Or maybe a more useful way to default to: run a set number of connections
// (like 2-4) for power, then 2 for every regular jumper, then fill in the rest
// of with more power connections.

void initializeYPositionLimits(void) {
  // Clear usage counters
  for (int i = 0; i < MAX_NETS; i++) {
    yPositionUsage[i] = 0;
    yPositionLimits[i] = 0; // 0 = no limit
  }

  // Set limits for high-priority nets to prevent them from monopolizing Y
  // positions Reserve at least 2 Y positions per chip for inter-chip hops
  yPositionLimits[1] =
      3; // GND - reduced to leave more room for inter-chip hops
  yPositionLimits[2] = 2; // Top Rail - limit to 2 positions
  yPositionLimits[3] = 2; // Bottom Rail - limit to 2 positions
  yPositionLimits[4] = 2; // DAC0 - limit to 2 positions
  yPositionLimits[5] = 2; // DAC1 - limit to 2 positions

  DEBUG_NTCC2_PRINTLN(
      "Y position limits initialized (reserving space for inter-chip hops):");
  DEBUG_NTCC2_PRINTLN("  GND (net 1): max 3 Y positions");
  DEBUG_NTCC2_PRINTLN("  Power rails: max 2 Y positions each");
  DEBUG_NTCC2_PRINTLN("  DACs: max 2 Y positions each");
}

bool canNetUseMoreYPositions(int net) {
  // Bounds checking to prevent crashes
  if (net < 0 || net >= MAX_NETS) {
    return true; // Invalid net, allow to prevent blocking
  }

  if (yPositionLimits[net] == 0) {
    return true; // No limit set
  }
  return yPositionUsage[net] < yPositionLimits[net];
}

// Function prototypes for forward declarations
bool assignYPositionWithTracking(int chip, int yPos, int net);
void setChipYStatus(int chip, int y, int net, const char *location);
bool resolveYPositionConflicts(void);
void validateNoYPositionOverlaps(void);

// bool assignYPositionWithTracking(int chip, int yPos, int net) {
//   // Bounds checking to prevent crashes
//   if (chip < 0 || chip >= 12 || yPos < 0 || yPos >= 8 || net < 0 || net >=
//   MAX_NETS) {
//     return false; // Invalid parameters, assignment failed
//   }

//   // Check for overlap conflicts before assignment
//   if (ch[chip].yStatus[yPos] != -1 && ch[chip].yStatus[yPos] != net) {
//     if (debugNTCC6) {
//       Serial.print("WARNING: Y position overlap detected! Chip ");
//       Serial.print(chipNumToChar(chip));
//       Serial.print(" Y[");
//       Serial.print(yPos);
//       Serial.print("] already occupied by net ");
//       Serial.print(ch[chip].yStatus[yPos]);
//       Serial.print(", trying to assign net ");
//       Serial.println(net);
//     }
//     // Don't overwrite - this could cause the overlap issue
//     return false; // Assignment failed due to conflict
//   }

//   // Only track new Y position usage if this Y position wasn't already used
//   by this net bool alreadyUsedByThisNet = false; if (ch[chip].yStatus[yPos]
//   == net) {
//     alreadyUsedByThisNet = true;
//   }

//   // Assign the Y position
//   ch[chip].yStatus[yPos] = net;

//   // Track usage if this is a new Y position for this net
//   if (!alreadyUsedByThisNet && yPositionLimits[net] > 0) {
//     // Count how many unique Y positions this net currently uses
//     int uniqueYPositions = 0;
//     for (int checkChip = 0; checkChip < 12; checkChip++) {
//       for (int checkY = 0; checkY < 8; checkY++) {
//         if (ch[checkChip].yStatus[checkY] == net) {
//           // Check if we've already counted this Y position
//           bool alreadyCounted = false;
//           for (int prevChip = 0; prevChip < checkChip; prevChip++) {
//             if (ch[prevChip].yStatus[checkY] == net) {
//               alreadyCounted = true;
//               break;
//             }
//           }
//           if (!alreadyCounted) {
//             uniqueYPositions++;
//           }
//         }
//       }
//     }
//     yPositionUsage[net] = uniqueYPositions;

//     if (debugNTCC2) {
//       Serial.print("  Assigned Y=");
//       Serial.print(yPos);
//       Serial.print(" on chip ");
//       Serial.print(chipNumToChar(chip));
//       Serial.print(" to net ");
//       Serial.print(net);
//       Serial.print(" (usage now: ");
//       Serial.print(yPositionUsage[net]);
//       Serial.print("/");
//       Serial.print(yPositionLimits[net]);
//       Serial.println(")");
//     }
//   }

//   return true; // Assignment successful
// }

void printYPositionUsageReport(void) {
  if (debugNTCC2) {
    Serial.println("\nY Position Usage Report:");
    Serial.println("Net\tName\t\tUsage/Limit");
    for (int net = 1; net < 6; net++) { // Check main power/special nets
      if (yPositionLimits[net] > 0) {
        Serial.print(net);
        Serial.print("\t");
        switch (net) {
        case 1:
          Serial.print("GND\t\t");
          break;
        case 2:
          Serial.print("Top Rail\t");
          break;
        case 3:
          Serial.print("Bottom Rail\t");
          break;
        case 4:
          Serial.print("DAC0\t\t");
          break;
        case 5:
          Serial.print("DAC1\t\t");
          break;
        default:
          Serial.print("Unknown\t\t");
          break;
        }
        Serial.print(yPositionUsage[net]);
        Serial.print("/");
        Serial.print(yPositionLimits[net]);
        if (yPositionUsage[net] >= yPositionLimits[net]) {
          Serial.print(" (LIMIT REACHED)");
        }
        Serial.println();
      }
    }
    Serial.println();
  }
}

bool resolveYPositionConflicts(void) {
  if (debugNTCC6) {
    Serial.println("\nResolving Y position conflicts (simple approach)...");
  }

  bool foundConflicts = false;

  // Simple approach: just look for paths that have overlapping Y positions
  for (int i = 0; i < numberOfPaths; i++) {
    if (path[i].skip)
      continue; // Skip already failed paths

    for (int j = i + 1; j < numberOfPaths; j++) {
      if (path[j].skip)
        continue; // Skip already failed paths
      if (path[i].net == path[j].net)
        continue; // Same net is OK

      // Check if these paths overlap on any chip
      for (int seg1 = 0; seg1 < 4; seg1++) {
        for (int seg2 = 0; seg2 < 4; seg2++) {
          if (path[i].chip[seg1] == path[j].chip[seg2] &&
              path[i].chip[seg1] != -1 && path[i].y[seg1] == path[j].y[seg2] &&
              path[i].y[seg1] > 0) {

            foundConflicts = true;
            if (debugNTCC6) {
              Serial.print("CONFLICT: Path ");
              Serial.print(i);
              Serial.print(" (net ");
              Serial.print(path[i].net);
              Serial.print(") and path ");
              Serial.print(j);
              Serial.print(" (net ");
              Serial.print(path[j].net);
              Serial.print(") both use chip ");
              Serial.print(chipNumToChar(path[i].chip[seg1]));
              Serial.print(" Y[");
              Serial.print(path[i].y[seg1]);
              Serial.println("]");
            }

            // Simple resolution: mark the higher-numbered path as unconnectable
            path[j].skip = true;
            if (debugNTCC6) {
              Serial.print("  Simple resolution: marked path[");
              Serial.print(j);
              Serial.println("] as unconnectable");
            }
            break;
          }
        }
        if (path[j].skip)
          break; // Don't check more if already marked
      }
    }
  }

  return foundConflicts;
}

void validateNoYPositionOverlaps(void) {
  if (debugNTCC6) {
    Serial.println("\nQuick Y position overlap check...");
    bool foundOverlaps = false;

    // Simple check: compare path pairs for overlaps
    for (int i = 0; i < numberOfPaths; i++) {
      if (path[i].skip)
        continue;

      for (int j = i + 1; j < numberOfPaths; j++) {
        if (path[j].skip)
          continue;
        if (path[i].net == path[j].net)
          continue; // Same net is OK

        // Quick check for any Y overlap
        for (int seg1 = 0; seg1 < 4; seg1++) {
          for (int seg2 = 0; seg2 < 4; seg2++) {
            if (path[i].chip[seg1] == path[j].chip[seg2] &&
                path[i].chip[seg1] != -1 &&
                path[i].y[seg1] == path[j].y[seg2] && path[i].y[seg1] > 0) {

              foundOverlaps = true;
              Serial.print("OVERLAP: Path ");
              Serial.print(i);
              Serial.print(" and ");
              Serial.print(j);
              Serial.print(" both use chip ");
              Serial.print(chipNumToChar(path[i].chip[seg1]));
              Serial.print(" Y[");
              Serial.print(path[i].y[seg1]);
              Serial.print("] nets ");
              Serial.print(path[i].net);
              Serial.print(" and ");
              Serial.println(path[j].net);
            }
          }
        }
      }
    }

    if (!foundOverlaps) {
      Serial.println("No Y position overlaps detected");
    }
  }
}

bool isGpioConnection(int pathIndex) {
  for (int i = 0; i < 10; i++) {
    if (path[pathIndex].node1 == gpioDef[i][0] ||
        path[pathIndex].node2 == gpioDef[i][1]) {
      return true;
    }
  }
  return false;
}

int getNetPriority(int netNumber) {
  // Bounds checking to prevent crashes
  if (netNumber < 0 || netNumber > MAX_NETS) {
    return 0;
  }

  return net[netNumber].priority;
}

void frontloadPriorityConnections(void) {
  DEBUG_NTCC2_PRINTLN("Frontloading connections by priority...");

  // Safety checks to prevent crashes
  if (numberOfPaths <= 0 || numberOfPaths > MAX_BRIDGES) {
    DEBUG_NTCC2_PRINT("Invalid numberOfPaths: ");
    DEBUG_NTCC2_PRINTLN(numberOfPaths);
    return;
  }

  // Use a simple in-place sorting approach to avoid large stack allocations
  // First, move all duplicates to the end
  int writeIndex = 0;

  // Pass 1: Copy non-duplicates to front, preserving order
  for (int i = 0; i < numberOfPaths; i++) {
    if (path[i].duplicate == 0) {
      if (writeIndex != i) {
        path[writeIndex] = path[i];
      }
      writeIndex++;
    }
  }

  // Store where duplicates should start
  int duplicateStartIndex = writeIndex;

  // Pass 2: Copy duplicates to end
  for (int i = 0; i < numberOfPaths; i++) {
    if (path[i].duplicate == 1) {
      path[writeIndex] = path[i];
      writeIndex++;
    }
  }

  // Now sort the non-duplicate section by priority
  // Simple insertion sort for priority paths (nets > 3 with priority > 1)
  for (int i = 1; i < duplicateStartIndex; i++) {
    pathStruct current = path[i];
    int currentPriority = (current.net > 3) ? getNetPriority(current.net) : 1;
    bool currentIsGpio =
        ((current.node1 >= NANO_D0 && current.node1 <= NANO_A7) ||
         (current.node2 >= NANO_D0 && current.node2 <= NANO_A7));

    int j = i - 1;

    // Move elements that should come after current path
    while (j >= 0) {
      int comparePriority = (path[j].net > 3) ? getNetPriority(path[j].net) : 1;
      bool compareIsGpio =
          ((path[j].node1 >= NANO_D0 && path[j].node1 <= NANO_A7) ||
           (path[j].node2 >= NANO_D0 && path[j].node2 <= NANO_A7));

      bool shouldMoveCurrentForward = false;

      // Higher priority should come first
      if (currentPriority > comparePriority) {
        shouldMoveCurrentForward = true;
      } else if (currentPriority == comparePriority) {
        // Same priority - GPIO connections get preference
        if (currentIsGpio && !compareIsGpio) {
          shouldMoveCurrentForward = true;
        }
      }

      if (!shouldMoveCurrentForward) {
        break;
      }

      path[j + 1] = path[j];
      j--;
    }

    path[j + 1] = current;
  }

  DEBUG_NTCC2_PRINT("Priority frontloading complete using in-place sort\n");

  // Count and show priority paths
#if DEBUG_NTCC2_ENABLED
  if (debugNTCC2) {
    int priorityCount = 0;
    for (int i = 0; i < duplicateStartIndex; i++) {
      if (path[i].net > 3 && getNetPriority(path[i].net) > 1) {
        priorityCount++;
      }
    }

    DEBUG_NTCC2_PRINT("Found ");
    DEBUG_NTCC2_PRINT(priorityCount);
    DEBUG_NTCC2_PRINT(" priority paths out of ");
    DEBUG_NTCC2_PRINT(duplicateStartIndex);
    DEBUG_NTCC2_PRINTLN(" non-duplicate paths");

    // Show the first few priority connections for verification
    if (priorityCount > 0) {
      DEBUG_NTCC2_PRINTLN("Top priority connections:");
      int shown = 0;
      for (int i = 0; i < duplicateStartIndex && shown < 5; i++) {
        if (path[i].net > 3 && getNetPriority(path[i].net) > 1) {
          DEBUG_NTCC2_PRINT("  [");
          DEBUG_NTCC2_PRINT(i);
          DEBUG_NTCC2_PRINT("] net ");
          DEBUG_NTCC2_PRINT(path[i].net);
          DEBUG_NTCC2_PRINT(" priority ");
          DEBUG_NTCC2_PRINT(getNetPriority(path[i].net));
          DEBUG_NTCC2_PRINT(": ");
          printNodeOrName(path[i].node1);
          DEBUG_NTCC2_PRINT("-");
          printNodeOrName(path[i].node2);
          if ((path[i].node1 >= NANO_D0 && path[i].node1 <= NANO_A7) ||
              (path[i].node2 >= NANO_D0 && path[i].node2 <= NANO_A7)) {
            DEBUG_NTCC2_PRINT(" [GPIO]");
          }
          DEBUG_NTCC2_PRINTLN();
          shown++;
        }
      }
    }
  }
#endif
}

void clearAllNTCC(void) {

  // digitalWrite(RESETPIN,HIGH);

  for (int i = 0; i < 12; i++) {
    chipsLeastToMostCrowded[i] = i;
  }
  for (int i = 0; i < 4; i++) {
    chipCandidates[0][i] = -1;
    chipCandidates[1][i] = -1;

    sfChipsLeastToMostCrowded[i] = i + 8;
  }
  // for (int g = 0; g < 10; g++) {
  // gpioNet[g] = -1;
  //   }
  for (int i = 0; i < numberOfPaths + 8; i++) {
    if (i >= MAX_BRIDGES) {
      break;
    }
    pathsWithCandidates[i] = 0;
    path[i].net = 0;
    path[i].node1 = 0;
    path[i].node2 = 0;
    path[i].altPathNeeded = false;
    path[i].sameChip = false;
    path[i].skip = false;

    for (int j = 0; j < 4; j++) {
      path[i].chip[j] = 0;
    }

    for (int j = 0; j < 6; j++) {
      path[i].x[j] = 0;
      path[i].y[j] = 0;
    }

    for (int j = 0; j < 3; j++) {
      path[i].nodeType[j] = BB;
      for (int k = 0; k < 3; k++) {
        path[i].candidates[j][k] = -1;
      }
    }
  }
  // //clang-format off
  // struct netStruct net[MAX_NETS] = { //these are the special function nets
  // that will always be made
  // //netNumber,       ,netName          ,memberNodes[] ,memberBridges[][2]
  // ,specialFunction        ,intsctNet[] ,doNotIntersectNodes[] ,priority
  // (unused)
  //     {     127      ,"Empty Net"      ,{EMPTY_NET}           ,{{}}
  //     ,EMPTY_NET              ,{}
  //     ,{EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET}
  //     , 0}, {     1        ,"GND"            ,{GND}                 ,{{}}
  //     ,GND                    ,{}          ,{SUPPLY_3V3,SUPPLY_5V,DAC0,DAC1}
  //     , 1}, {     2        ,"Top Rail"       ,{TOP_RAIL}            ,{{}}
  //     ,TOP_RAIL               ,{}          ,{GND} , 1}, {     3 ,"Bottom
  //     Rail"    ,{BOTTOM_RAIL}         ,{{}}                   ,BOTTOM_RAIL
  //     ,{}          ,{GND}                               , 1}, {     4 ,"DAC
  //     0"          ,{DAC0}                ,{{}}                   ,DAC0 ,{}
  //     ,{GND}                               , 1}, {     5        ,"DAC 1"
  //     ,{DAC1}                ,{{}}                   ,DAC1 ,{} ,{GND} , 1},
  //     {     6        ,"I Sense +"      ,{ISENSE_PLUS}         ,{{}}
  //     ,ISENSE_PLUS            ,{}          ,{ISENSE_MINUS} , 2}, {     7 ,"I
  //     Sense -"      ,{ISENSE_MINUS}        ,{{}} ,ISENSE_MINUS           ,{}
  //     ,{ISENSE_PLUS}                       , 2},
  // };
  net[0] = {127,
            "Empty Net",
            {EMPTY_NET},
            {{}},
            EMPTY_NET,
            {},
            {EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET,
             EMPTY_NET},
            0};
  net[1] = {1, "GND", {GND}, {{}}, GND, {}, {SUPPLY_3V3, SUPPLY_5V, DAC0, DAC1},
            1};
  net[2] = {2, "Top Rail", {TOP_RAIL}, {{}}, TOP_RAIL, {}, {GND}, 1};
  net[3] = {3, "Bottom Rail", {BOTTOM_RAIL}, {{}}, BOTTOM_RAIL, {}, {GND}, 1};
  net[4] = {4, "DAC 0", {DAC0}, {{}}, DAC0, {}, {GND}, 1};
  net[5] = {5, "DAC 1", {DAC1}, {{}}, DAC1, {}, {GND}, 1};

  //clang-format on

  initNets();
  initializeYPositionLimits();

  for (int i = 0; i < 12; i++) {
    ch[i].uncommittedHops = 0;
    for (int j = 0; j < 16; j++) {
      ch[i].xStatus[j] = -1;
    }

    for (int j = 0; j < 8; j++) {
      ch[i].yStatus[j] = -1;
    }
  }
  for (int i = 0; i < 10; i++) {
    unconnectablePaths[i][0] = -1;
    unconnectablePaths[i][1] = -1;
  }
  numberOfUnconnectablePaths = 0;
  // printPathsCompact();
  // printChipStatus();

  for (int i = 0; i < 8; i++) {
    if (gpioNet[i] != -2) {
      gpioNet[i] = -1;
    }
    showADCreadings[i] = -1;
    gpioReading[i] = 3;
    gpioReadingColors[i] = 0x010101;
  }
  if (gpioNet[8] != -2) {
    gpioNet[8] = -1;
  }
  if (gpioNet[9] != -2) {
    gpioNet[9] = -1;
  }
  gpioReading[8] = 3;
  gpioReading[9] = 3;
  gpioReadingColors[8] = 0x010101;
  gpioReadingColors[9] = 0x010101;

  startEndChip[0] = -1;
  startEndChip[1] = -1;
  bothNodes[0] = -1;

  bothNodes[1] = -1;

  numberOfUniqueNets = 0;
  numberOfNets = 0;
  numberOfPaths = 0;

  pathsWithCandidatesIndex = 0;
  pathIndex = 0;
  // findChangedNetColors();
  //  for (int i = 0; i<MAX_NETS; i++) {
  //   // changedNetColors[i] = 0;
  //  }
  // digitalWrite(RESETPIN,LOW);
}

void sortPathsByNet(
    void) // not actually sorting, just copying the bridges and nets back from
// netStruct so they're both in the same order
{
  if (debugNTCC) {
    Serial.println("sortPathsByNet()");
  }
  timeToSort = micros();
  numberOfPaths = 0;
  pathIndex = 0;

  if (debugNTCC) {
    printBridgeArray();
  }

  numberOfNets = 1;
  for (int i = 1; i < MAX_NETS - 1; i++) {
    if (net[i].number == 0 || net[i].number == -1) {
      break;
      // continue;
    } else {
      numberOfNets++;
      // break;
    }
  }

  for (int i = 0; i < MAX_BRIDGES; i++) {
    if ((path[i].node1 != 0 && path[i].node2 != 0) &&
        (path[i].node1 != -1 && path[i].node2 != -1)) {
      numberOfPaths++;

      // Serial.print("path[");
      // Serial.print(i);
      // Serial.print("] ");
      // Serial.print("node1: ");
      // Serial.print(path[i].node1);
      // Serial.print("  node2: ");
      // Serial.println(path[i].node2);

      // break;
    } else if (path[i].node1 == 0 || path[i].node2 == 0) {
      break;
    }
  }

  // Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // printPathArray();
  // if (debugNTCC)
  // {
  // Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // }

  int routableBufferPowerFound = -1;

  int lastPowerPath = -1;
  numberOfUniqueNets = 0;
  numberOfShownNets = 0;

  for (int j = 1; j <= MAX_NETS; j++) {
    if (net[j].number == 0) {
      break;
      // continue;
    }

    for (int k = 0; k < MAX_NODES; k++) {
      if (net[j].bridges[k][0] == 0) {
        break;
        // continue;
      } else {
        path[pathIndex].net = net[j].number;
        path[pathIndex].node1 = net[j].bridges[k][0];
        path[pathIndex].node2 = net[j].bridges[k][1];
        path[pathIndex].duplicate = 0;
        indexByNet[pathIndex] = pathIndex;

        if (probePowerDAC == 0) {
          if ((path[pathIndex].node1 == ROUTABLE_BUFFER_IN &&
               path[pathIndex].node2 == DAC0) ||
              (path[pathIndex].node1 == DAC0 &&
               path[pathIndex].node2 == ROUTABLE_BUFFER_IN)) {
            routableBufferPowerFound = pathIndex;
          }
        } else if (probePowerDAC == 1) {
          if ((path[pathIndex].node1 == ROUTABLE_BUFFER_IN &&
               path[pathIndex].node2 == DAC1) ||
              (path[pathIndex].node1 == DAC1 &&
               path[pathIndex].node2 == ROUTABLE_BUFFER_IN)) {
            routableBufferPowerFound = pathIndex;
          }
        }
        if (path[pathIndex].net <= 5) {
          lastPowerPath = pathIndex;
        }

        if (path[pathIndex].net == path[pathIndex - 1].net) {
        } else {
          numberOfUniqueNets++;
          if (path[pathIndex].net >= 6) {
            if ((path[pathIndex].node1 <= 60 ||
                 (path[pathIndex].node1 >= NANO_D0 &&
                  path[pathIndex].node1 <= NANO_RESET_1)) ||
                (path[pathIndex].node2 <= 60 ||
                 (path[pathIndex].node2 >= NANO_D0 &&
                  path[pathIndex].node2 <= NANO_RESET_1))) {
              net[j].visible = 1;
              numberOfShownNets++;
              // Serial.print("path  ");
              // Serial.print(pathIndex);
              // Serial.print("   net ");
              // Serial.print(j);
              // Serial.println(" is visible\n\r");

            } else {

              net[j].visible = 0;
              // Serial.print("path  ");
              // Serial.print(pathIndex);
              // Serial.print("   net ");
              // Serial.print(j);
              // Serial.print(" is not visible\n\r");
              // Serial.print("node1: ");
              // Serial.print(path[pathIndex].node1);
              // Serial.print("  node2: ");
              // Serial.println(path[pathIndex].node2);
            }
            // numberOfShownNets++;
          }
        }

        // if (debugNTCC) {
        // Serial.print("path[");
        // Serial.print(pathIndex);
        // Serial.print("] net: ");
        // Serial.println(path[pathIndex].net);
        //}
        pathIndex++;
      }
    }
  }

  if (routableBufferPowerFound > 0) {

    pathStruct tempPath = path[routableBufferPowerFound];

    // printPathsCompact();

    // shift all paths up one and put routableBufferPower path at the beginning
    for (int i = routableBufferPowerFound; i > 0; i--) {
      path[i] = path[i - 1];
    }
    path[0] = tempPath;

    // printPathsCompact();
  }
  // Serial.print("Routable Buffer Power Found: ");
  // Serial.println(routableBufferPowerFound);

  // Serial.print("Last Power Path: ");
  // Serial.println(lastPowerPath);

  newBridgeLength = numberOfPaths;
  numberOfPaths = pathIndex;

  // for (int i = 0; i < numberOfNets; i++) {

  //   }

  if (debugNTCC) {
    Serial.print("number unique of nets: ");
    Serial.println(numberOfUniqueNets);
    Serial.print("pathIndex: ");
    Serial.println(pathIndex);
    Serial.print("numberOfPaths: ");
    Serial.println(numberOfPaths);
  }
  // numberOfShownNets = numberOfUniqueNets;
  //  printPathArray();
  clearChipsOnPathToNegOne(); // clear chips and all trailing paths to -1{if
  // there are bridges that weren't made due to DNI
  // rules, there will be fewer paths now because
  // they were skipped}

  if (debugNTCC) {
    Serial.println("cleared trailing paths");
    // delay(10);
    printBridgeArray();
    // delay(10);
    Serial.println("\n\r");
    timeToSort = micros() - timeToSort;
    Serial.print("time to sort: ");
    Serial.print(timeToSort);
    Serial.println("us\n\r");
  }
}

void bridgesToPaths(
    int fillUnused,
    int allowStacking) { ///!this is the main function that gets called
  if (debugNTCC5) {
    Serial.println("bridgesToPaths()");
  }

  for (int i = 0; i < MAX_BRIDGES; i++) {
    pathsWithCandidates[i] = 0;
  }
  int duplicateStartIndex = 0;
  // allowStacking = 0;
  sortPathsByNet();

  // Frontload connections by priority routing
 //frontloadPriorityConnections();

  DEBUG_NTCC2_PRINT("After priority frontloading - total paths: ");
  DEBUG_NTCC2_PRINTLN(numberOfPaths);
  //   Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // Serial.print("number of shown paths: ");
  // Serial.println(numberOfShownNets);
  // printPathsCompact(2);

  // Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // Serial.print("number of shown paths: ");
  // Serial.println(numberOfShownNets);
  // sortPathsByNet();

  //   if (fillUnused == 1) {
  // fillUnusedPaths(jumperlessConfig.routing.stack_paths,
  // jumperlessConfig.routing.stack_rails, jumperlessConfig.routing.stack_dacs);
  // }

  // printPathsCompact(2);
  // printChipStatus();
  duplicateStartIndex = numberOfPaths;

  for (int i = 0; i < numberOfPaths; i++) {
    if (path[i].duplicate == 1) {
      continue;
    }

    if (debugNTCC5) {
      delay(10);
      Serial.print("path[");
      Serial.print(i);
      Serial.print("]\n\rnodes [");
      Serial.print(path[i].node1);
      Serial.print("-");
      Serial.print(path[i].node2);
      Serial.println("]\n\r");
    }

    findStartAndEndChips(path[i].node1, path[i].node2, i);

    if (debugNTCC5) {
      delay(10);
      Serial.print("startEndChip[0]: ");
      Serial.print(startEndChip[0]);
      Serial.print("  startEndChip[1]: ");
      Serial.println(startEndChip[1]);
    }

    mergeOverlappingCandidates(i);
    if (debugNTCC5) {
      delay(10);
      Serial.println("mergeOverlappingCandidates done");
    }

    assignPathType(i);

    if (debugNTCC5) {
      delay(10);
      Serial.println("assignPathType done");
      Serial.println("\n\n\r");
    }
  }

  if (debugNTCC5) {
    Serial.println("paths with candidates:");
  }

  if (debugNTCC5) {
    delay(10);
    for (int i = 0; i < pathsWithCandidatesIndex; i++) {
      Serial.print(pathsWithCandidates[i]);
      Serial.print(",");
    }
    Serial.println("\n\r");
    // printPathArray();
  }

  sortAllChipsLeastToMostCrowded();

  resolveChipCandidates();
  // Serial.print("Allow Stacking: ");
  // Serial.println(allowStacking);

  // commitPaths(0, -1, 0);

  // resolveAltPaths(0, -1, 0);

  // resolveUncommittedHops(0, -1, 0);

  commitPaths(2, -1, 0);
  // printPathsCompact(2);
  // printChipStatus();

  resolveAltPaths(2, -1, 0);
  // printPathsCompact(2);
  // printChipStatus();
  resolveUncommittedHops(2, -1, 0);
  // printPathsCompact(2 );
  // printChipStatus();
  // Serial.println("no duplicates");

  // printPathsCompact(2);
  // printChipStatus();

  // Serial.println("fillUnused: ");
  // Serial.println(fillUnused);

  if (fillUnused == 1) {
    fillUnusedPaths(jumperlessConfig.routing.stack_paths,
                    jumperlessConfig.routing.stack_rails,
                    jumperlessConfig.routing.stack_dacs);

    // printPathsCompact(2);
    // printChipStatus();

    for (int i = duplicateStartIndex; i < numberOfPaths; i++) {
      if (path[i].duplicate == 0) {
        continue;
      }
      findStartAndEndChips(path[i].node1, path[i].node2, i);
      mergeOverlappingCandidates(i);
      assignPathType(i);
    }

    // printPathsCompact(2);
    // printChipStatus();
    commitPaths(0, -1, 1);
    resolveAltPaths(0, -1, 1);

    resolveUncommittedHops(0, -1, 1);
  }

  couldntFindPath(1);
  // couldntFindPath();
  checkForOverlappingPaths();
  // Serial.println("only duplicates");
  // printPathsCompact();
  // printChipStatus();

  //   printPathsCompact(2 );
  // printChipStatus();
#if DEBUG_NTCC2_ENABLED
  if (debugNTCC2) {
    // delay(10);
    printPathsCompact(2);
    // delay(10);
    printChipStatus();
    // delay(10);
    printYPositionUsageReport();
  }
#endif

  // Resolve any Y position conflicts before final validation
  // if (resolveYPositionConflicts()) {
  //   if (debugNTCC6) {
  //     Serial.println("Conflicts were found and resolved - re-validating...");
  //   }
  // }

  // Detect and report any remaining conflicts
#if DEBUG_NTCC6_ENABLED
  detectAndReportConflicts();
#endif

  // Validate transaction consistency
#if DEBUG_NTCC6_ENABLED
  // validateTransactionConsistency();
#endif
}

void fillUnusedPaths(int duplicatePathsOverride, int duplicatePathsPower,
                     int duplicatePathsDac) {
  /// return;

  int duplicatePathIndex = 0;

  uint8_t nodeCount[MAX_NETS] = {0};
  uint8_t bridgeCount[MAX_NETS] = {0};

  for (int i = 0; i < MAX_NETS; i++) {
    for (int j = 0; j < MAX_DUPLICATE; j++) {
      for (int k = 0; k < 2; k++) {
        newBridges[i][j][k] = 0;
      }
    }
  }

    //   Serial.print("numberOfNets: ");
    // Serial.println(numberOfNets);
    // Serial.print("net[");


  for (int n = 0; n < numberOfNets; n++) {

    // Serial.print(n);
    // Serial.print("] \n\rnumber: ");
    // Serial.println(net[n].number);

    for (int i = 0; i < MAX_NODES; i++) {
      if (net[n].nodes[i] == 0) {
        break;
      }
      nodeCount[n]++;
      // Serial.print(" \n\rnode: ");
      // Serial.println(net[n].nodes[i]);
    }

    for (int i = 0; i < MAX_BRIDGES; i++) {
      if (net[n].bridges[i][0] == 0) {
        break;
      }
      bridgeCount[n]++;
      // Serial.print(" \n\rbridges: ");
      // Serial.print(net[n].bridges[i][0]);
      // Serial.print("-");
      // Serial.println(net[n].bridges[i][1]);
    }
    // Serial.println("\n\r");
  }

  int duplindex = 0;
  // first figure out which paths need duplicates
  // Set duplicates once per net, not once per path to avoid exponential
  // duplication
  bool netProcessed[MAX_NETS] = {false};
  for (int i = 0; i < numberOfPaths; i++) {
    if (path[i].net > 0 &&
        !netProcessed[path[i].net]) { // Only process each net once
      netProcessed[path[i].net] = true;
      if (path[i].net <= 3) {
        net[path[i].net].numberOfDuplicates = duplicatePathsPower;

      } else if (path[i].net == 4 || path[i].net == 5) {
        net[path[i].net].numberOfDuplicates = duplicatePathsDac;

      } else {
        int isGpioNet = 0;
        for (int g = 0; g < 10; g++) {//dont duplicate gpio nets
          if (gpioNet[g] == path[i].net) {
            isGpioNet = 1;
            break;
          }
        }
        if (isGpioNet == 0) {
          net[path[i].net].numberOfDuplicates =
              jumperlessConfig.routing.stack_paths;
        } else {
          net[path[i].net].numberOfDuplicates = 0;
        }
      }
    }

    // Serial.print("net[");
    // Serial.print(path[i].net);
    // Serial.print("] numberOfDuplicates: ");
    // Serial.println(net[path[i].net].numberOfDuplicates);
  }

  // get the nodes in the net and cycle them, so if the bridges are A-B, B-C,
  // the duplicate paths will start with A-C

  //  A-B, B-C                        -> A-C
  //  A-B, B-C, C-D                   -> A-C, A-D, B-D
  //  A-B, B-C, C-D, D-E              -> A-C, A-D, A-E, B-D, B-E,
  //  A-B, B-C, C-D, D-E, E-F         -> A-C, A-D, A-E, A-F, B-D, B-E, B-F, C-E,
  //  C-F, D-F A-B, B-C, C-D, D-E, E-F, F-G    -> A-C, A-D, A-E, A-F, A-G, B-D,
  //  B-E, B-F, B-G, C-E, C-F, C-G, D-F, D-G, E-G A-B, B-C, C-D, D-E, E-F, F-G,
  //  G-H -> A-C, A-D, A-E, A-F, A-G, A-H, B-D, B-E, B-F, B-G, B-H, C-E, C-F,
  //  C-G, C-H, D-F, D-G, D-H, E-G, E-H, F-H

  // int bridgeLUT[MAX_DUPLICATE] = {1, 1, 3, 5, 10, 15, 21, 28, 36, 45, 55, 66,
  // 78, 91, 105, 120, 136, 153, 171, 190, 210, 231, 253, 276, 300};

  // int16_t tempNodes[MAX_NETS][MAX_NODES] = {0};

  for (int i = 1; i < numberOfNets; i++) {
    if (net[i].numberOfDuplicates == 0) {
      // Serial.print("net[");
      // Serial.print(i);
      // Serial.println("] numberOfDuplicates is 0");
      continue;
    }

    // int16_t tempNodes[MAX_NODES];
    //  Serial.print("net[");
    //  Serial.print(i);
    //  Serial.print("]  nodes[");

    for (int j = 0; j < nodeCount[i]; j++) {
      // tempNodes[j] = net[i].nodes[j];
      //   Serial.print(tempNodes[j]);
      // Serial.print(net[i].nodes[j]);
      // Serial.print(", ");
    }
    // Serial.println("]\t\t");

    int targetBridgeCount = net[i].numberOfDuplicates;
    int skip = 1;

    int unique = 0;

    int testCounter0 = 0;
    int testCounter1 = 1; // nodeCount[i] / 2;
    int testBridge[2] = {-1, -1};

    int bridge0 = 0;
    int bridge1 = 1;

    for (int j = 0; j < targetBridgeCount; j++) {
      if (nodeCount[i] >= 3) {
        for (int l = 0; l < MAX_DUPLICATE; l++) {
          if (unique == -1) {
            bridge1++;
            if (bridge1 >= nodeCount[i]) {
              bridge0++;
              if (bridge0 >= nodeCount[i]) {
                bridge0 = 0;
              }
              bridge1 = bridge0 + 1;
            }
            unique = 0;
          }
          if (net[i].nodes[bridge0] == 0 || net[i].nodes[bridge1] == 0) {
            break;
          }
          if (net[i].nodes[bridge0] == net[i].nodes[bridge1]) {
            unique = -1;
            continue;
          }
          if (net[i].nodes[bridge0] == net[i].bridges[l][0] &&
              net[i].nodes[bridge1] == net[i].bridges[l][1]) {
            unique = -1;
            continue;
          }
          if (net[i].nodes[bridge0] == net[i].bridges[l][1] &&
              net[i].nodes[bridge1] == net[i].bridges[l][0]) {
            unique = -1;
            continue;
          }
          unique = 1;
          // Serial.print(net[i].nodes[bridge0]);
          // Serial.print("-");
          // Serial.print(net[i].nodes[bridge1]);
          // Serial.println("]\t\t");
          // Serial.print("net[");

          break;
        }
      }
      newBridges[i][j][0] = net[i].nodes[bridge0];
      newBridges[i][j][1] = net[i].nodes[bridge1];

      net[i].bridges[j][0] = newBridges[i][j][0];
      net[i].bridges[j][1] = newBridges[i][j][1];

      bridge1++;

      if (bridge1 >= nodeCount[i]) {
        bridge0++;
        if (bridge0 >= nodeCount[i]) {
          bridge0 = 0;
        }
        bridge1 = bridge0 + 1;
      }

      if (newBridges[i][j][0] == newBridges[i][j][1] ||
          newBridges[i][j][0] == 0 || newBridges[i][j][1] == 0) {
        // Serial.print("skipping ");
        // Serial.println(j);
        j--;
        continue;
      } else {
        duplicatePathIndex++;
      }
    }
  }
  // int maxxed = 0;
  int priorities[MAX_NETS] = {0};
  int maxp = 0;

  for (int j = 0; j < MAX_DUPLICATE; j++) {
    for (int i = 0; i < numberOfNets; i++) {
      priorities[i] = net[i].priority;
      if (i < 6 && net[i].priority > maxp) {
        maxp = net[i].priority;
      }
    }
    for (int k = 0; k < maxp; k++) {
      for (int i = 0; i < 6; i++) {
        // for (int p = 0; p < net[i].priority; p++) {

        if (net[i].numberOfDuplicates == 0) {
          continue;
        }

        // if (newBridges[i][j][0] >= 110 && newBridges[i][j][0] <= 115 ||
        //     newBridges[i][j][1] >= 110 && newBridges[i][j][1] <= 115) {
        //   continue;
        // }

        if (priorities[i] < 0) { ///!
          continue;
        }

        if (priorities[i] > 0) {
          priorities[i]--;
        }

        //! make it add the the priority so the connections are mixed
        if (probePowerDAC == 0) {
          if (newBridges[i][j][0] == ROUTABLE_BUFFER_IN &&
                  newBridges[i][j][1] == DAC0 ||
              newBridges[i][j][0] == DAC0 &&
                  newBridges[i][j][1] == ROUTABLE_BUFFER_IN) {
            continue;
          }
        } else if (probePowerDAC == 1) {
          if (newBridges[i][j][0] == ROUTABLE_BUFFER_IN &&
                  newBridges[i][j][1] == DAC1 ||
              newBridges[i][j][0] == DAC1 &&
                  newBridges[i][j][1] == ROUTABLE_BUFFER_IN) {
            continue;
          }
        }
        if (newBridges[i][j][0] != 0 || newBridges[i][j][1] != 0) {
          path[numberOfPaths].net = i;
          path[numberOfPaths].node1 = newBridges[i][j][0];
          path[numberOfPaths].node2 = newBridges[i][j][1];
          path[numberOfPaths].altPathNeeded = false;
          path[numberOfPaths].sameChip = false;
          path[numberOfPaths].skip = false;
          path[numberOfPaths].duplicate = 1;
          numberOfPaths++;
          if (numberOfPaths >= MAX_BRIDGES) {
            // maxxed = 1;
            return;
            break;
          }
        }
        // }
        // Serial.print("\n\r");
      }
    }

    // for (int i = 0; i < 6; i++) {
    //   priorities[i] = net[i].priority;
    // }
    for (int i = 5; i < numberOfNets; i++) {
      if (net[i].numberOfDuplicates == 0) {
        continue;
      }

      if (newBridges[i][j][0] >= 110 && newBridges[i][j][0] <= 115 ||
          newBridges[i][j][1] >= 110 && newBridges[i][j][1] <= 115) {
        continue;
      }

      if (priorities[i] <= 0) {
        continue;
      }

      if (priorities[i] > 0) {
        priorities[i]--;
      }

      if (newBridges[i][j][0] != 0 && newBridges[i][j][1] != 0) {
        ///
        net[i].bridges[bridgeCount[i]][0] = newBridges[i][j][0];
        net[i].bridges[bridgeCount[i]][1] = newBridges[i][j][1];
        bridgeCount[i]++; ///!why is this incrementing bridgeCount[0]?

        path[numberOfPaths].net = i;
        path[numberOfPaths].node1 = newBridges[i][j][0];
        path[numberOfPaths].node2 = newBridges[i][j][1];
        path[numberOfPaths].altPathNeeded = false;
        path[numberOfPaths].sameChip = false;
        path[numberOfPaths].skip = false;
        path[numberOfPaths].duplicate = 1;
        numberOfPaths++;
        if (numberOfPaths >= MAX_BRIDGES) {
          // maxxed = 1;
          return;
          break;
        }
      }
      // }
      // Serial.print("\n\r");
    }
  }
  // Serial.print("done filling unused paths\n\r");
}

void commitPaths(int allowStacking, int powerOnly, int noOrOnlyDuplicates) {
  DEBUG_NTCC2_PRINTLN("commitPaths()\n\r");

  for (int i = 0; i < numberOfPaths; i++) {
    // duplicateSFnets();
    // Serial.print(i);
    // Serial.print(" \t");

    DEBUG_NTCC1_PRINT("\n\rpath[");
    DEBUG_NTCC1_PRINT(i);
    DEBUG_NTCC1_PRINT("] net: ");
    DEBUG_NTCC1_PRINT(path[i].net);
    DEBUG_NTCC1_PRINT("   \t ");

    if (debugNTCC) {
      printNodeOrName(path[i].node1);
    }
    DEBUG_NTCC1_PRINT(" to ");
    if (debugNTCC) {
      printNodeOrName(path[i].node2);
    }

    // Skip paths that are already successfully committed (x values set and no
    // altPath needed)
    if (path[i].x[0] >= 0 && path[i].x[1] >= 0 && !path[i].altPathNeeded) {
      continue; // Path already committed successfully
    }

    if (powerOnly == 1 && path[i].net > 5) {
      continue;
    }
    if (powerOnly == 1 && path[i].duplicate == 1) {
      continue;
    }

    if (noOrOnlyDuplicates == 1 && path[i].duplicate == 0) {
      continue;
    }

    if (noOrOnlyDuplicates == 0 && path[i].duplicate == 1) {
      continue;
    }

    // Try path first without stacking, then with stacking if needed (only if
    // allowStacking is 2)
    bool pathCommitted = false;
    int maxStackingAttempts =
        (allowStacking == 2) ? 1 : 0; // 0-1 if allowStacking==2, 0-0 otherwise

    // Store original path state for retry attempts
    bool originalAltPathNeeded = path[i].altPathNeeded;

    for (int stackingAttempt = 0;
         stackingAttempt <= maxStackingAttempts && !pathCommitted;
         stackingAttempt++) {
      int currentAllowStacking =
          (allowStacking == 2) ? stackingAttempt : allowStacking;

      // Reset altPathNeeded for retry attempt
      if (stackingAttempt == 1) {
        path[i].altPathNeeded = originalAltPathNeeded; // Restore original state
      }

      switch (path[i].pathType) {
      case BBtoBB: {
        // Serial.print("BBtoBB\t");
        int freeLane = -1;
        int xMapL0c0 = xMapForChipLane0(path[i].chip[0], path[i].chip[1]);
        int xMapL1c0 = xMapForChipLane1(path[i].chip[0], path[i].chip[1]);

        int xMapL0c1 = xMapForChipLane0(path[i].chip[1], path[i].chip[0]);
        int xMapL1c1 = xMapForChipLane1(path[i].chip[1], path[i].chip[0]);

        if (path[i].chip[0] ==
            path[i].chip[1]) { // && (ch[path[i].chip[0]].yStatus[0] == -1)) {
          // if (path[i].sameChip == true ){
          //  Serial.print("same chip  ");
          setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
          setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[0]));
          setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                             "same-chip Y0");
          setChipYStatusSafe(path[i].chip[0], path[i].y[1], path[i].net,
                             "same-chip Y1");
          setPathX(i, 0, -2);
          setPathX(i, 1, -2);

          if (debugNTCC == true) {
            Serial.print("path [");
            Serial.print(i);
            Serial.print("]  ");

            Serial.print(" \tchip[0]: ");
            Serial.print(chipNumToChar(path[i].chip[0]));

            Serial.print("  x[0]: ");
            Serial.print(path[i].x[0]);

            Serial.print("  y[0]: ");
            Serial.print(path[i].y[0]);

            Serial.print("\t  chip[1]: ");
            Serial.print(chipNumToChar(path[i].chip[1]));

            Serial.print("  x[1]: ");
            Serial.print(path[i].x[1]);

            Serial.print("  y[1]: ");
            Serial.print(path[i].y[1]);
          }

          pathCommitted = true;
          break;
        }

        DEBUG_NTCC2_PRINT("xMapL0c0: ");
        DEBUG_NTCC2_PRINTLN(xMapL0c0);
        DEBUG_NTCC2_PRINT("xMapL0c1: ");
        DEBUG_NTCC2_PRINTLN(xMapL0c1);
        DEBUG_NTCC2_PRINT("xMapL1c0: ");
        DEBUG_NTCC2_PRINTLN(xMapL1c0);
        DEBUG_NTCC2_PRINT("xMapL1c1: ");
        DEBUG_NTCC2_PRINTLN(xMapL1c1);

        // Check if we can commit this path without conflicts
        bool canCommitLane0 =
            freeOrSameNetX(path[i].chip[0], xMapL0c0, path[i].net,
                           currentAllowStacking) &&
            freeOrSameNetX(path[i].chip[1], xMapL0c1, path[i].net,
                           currentAllowStacking);
        bool canCommitLane1 =
            (xMapL1c0 != -1) &&
            freeOrSameNetX(path[i].chip[0], xMapL1c0, path[i].net,
                           currentAllowStacking) &&
            freeOrSameNetX(path[i].chip[1], xMapL1c1, path[i].net,
                           currentAllowStacking);

        if (canCommitLane0) {
          freeLane = 0;
        } else if (canCommitLane1) {
          freeLane = 1;
        } else {
          path[i].altPathNeeded = true;
          DEBUG_NTCC2_PRINT(
              "\tno free lanes for path, setting altPathNeeded flag");
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(ch[path[i].chip[0]].xStatus[xMapL0c0]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(ch[path[i].chip[0]].xStatus[xMapL1c0]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(ch[path[i].chip[1]].xStatus[xMapL0c1]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(ch[path[i].chip[1]].xStatus[xMapL1c1]);
          DEBUG_NTCC2_PRINTLN(" \t ");
          break;
        }

        // Save state before making any assignments
        saveRoutingState(i);

        bool allAssignmentsSuccessful = false;

        // Only commit chip state after validating ALL connections
        if (freeLane == 0) {
          bool x0Success = setChipXStatus(path[i].chip[0], xMapL0c0,
                                          path[i].net, "BBtoBB lane0 chip0");
          bool x1Success = setChipXStatus(path[i].chip[1], xMapL0c1,
                                          path[i].net, "BBtoBB lane0 chip1");
          bool pathX0Success = setPathX(i, 0, xMapL0c0);
          bool pathX1Success = setPathX(i, 1, xMapL0c1);

          if (!x0Success || !x1Success || !pathX0Success || !pathX1Success) {
            if (debugNTCC6) {
              Serial.print("BBtoBB lane0 X assignment failed for path[");
              Serial.print(i);
              Serial.println("], will restore state");
            }
            allAssignmentsSuccessful = false;
          } else {
            allAssignmentsSuccessful = true;
          }
        } else if (freeLane == 1) {
          bool x0Success = setChipXStatus(path[i].chip[0], xMapL1c0,
                                          path[i].net, "BBtoBB lane1 chip0");
          bool x1Success = setChipXStatus(path[i].chip[1], xMapL1c1,
                                          path[i].net, "BBtoBB lane1 chip1");
          bool pathX0Success = setPathX(i, 0, xMapL1c0);
          bool pathX1Success = setPathX(i, 1, xMapL1c1);

          if (!x0Success || !x1Success || !pathX0Success || !pathX1Success) {
            if (debugNTCC6) {
              Serial.print("BBtoBB lane1 X assignment failed for path[");
              Serial.print(i);
              Serial.println("], will restore state");
            }
            allAssignmentsSuccessful = false;
          } else {
            allAssignmentsSuccessful = true;
          }
        }

        // Continue with Y assignments only if X assignments succeeded
        if (allAssignmentsSuccessful) {
          bool y0PathSuccess =
              setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
          bool y1PathSuccess =
              setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[1]));

          // Check if Y position assignments succeed
          bool y0ChipSuccess = false;
          bool y1ChipSuccess = false;

          if (y0PathSuccess) {
            y0ChipSuccess =
                setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                   "BBtoBB Y0 assignment");
          }
          if (y1PathSuccess) {
            y1ChipSuccess =
                setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                   "BBtoBB Y1 assignment");
          }

          if (!y0PathSuccess || !y1PathSuccess || !y0ChipSuccess ||
              !y1ChipSuccess) {
            allAssignmentsSuccessful = false;
            if (debugNTCC6) {
              Serial.print("Path[");
              Serial.print(i);
              Serial.print("] net ");
              Serial.print(path[i].net);
              Serial.print(" Y assignment failed");
              if (!y0PathSuccess || !y0ChipSuccess) {
                Serial.print(" (Y0=");
                Serial.print(path[i].y[0]);
                Serial.print(" on chip ");
                Serial.print(chipNumToChar(path[i].chip[0]));
                Serial.print(" failed)");
              }
              if (!y1PathSuccess || !y1ChipSuccess) {
                Serial.print(" (Y1=");
                Serial.print(path[i].y[1]);
                Serial.print(" on chip ");
                Serial.print(chipNumToChar(path[i].chip[1]));
                Serial.print(" failed)");
              }
              Serial.println(", will restore state");
            }
          }
        }

        // Handle success or failure
        if (allAssignmentsSuccessful) {
          commitRoutingState();
          if (debugNTCC6) {
            Serial.print("BBtoBB path[");
            Serial.print(i);
            Serial.println("] assignments committed successfully");
          }
        } else {
          restoreRoutingState(i);
          path[i].altPathNeeded = true;
          if (debugNTCC6) {
            Serial.print("BBtoBB path[");
            Serial.print(i);
            Serial.println(
                "] assignments failed, state restored, marked for alt routing");
          }
          break;
        }

        if (debugNTCC2 == true) {
          Serial.print(" \tchip[0]: ");
          Serial.print(chipNumToChar(path[i].chip[0]));

          Serial.print("  x[0]: ");
          Serial.print(path[i].x[0]);

          Serial.print("  y[0]: ");
          Serial.print(path[i].y[0]);

          Serial.print("\t  chip[1]: ");
          Serial.print(chipNumToChar(path[i].chip[1]));

          Serial.print("  x[1]: ");
          Serial.print(path[i].x[1]);

          Serial.print("  y[1]: ");
          Serial.print(path[i].y[1]);

          Serial.print(" \t ");
          Serial.print(ch[path[i].chip[0]].xStatus[xMapL0c0]);

          Serial.print(" \t ");
          Serial.print(ch[path[i].chip[1]].xStatus[xMapL0c1]);
          Serial.print(" \t ");
        }
        pathCommitted = true;
        break;
      }

      case BBtoNANO:
      case BBtoSF: // nodes should always be in order of the enum, so node1 is
                   // BB and node2 is SF
      {
        int xMapBBc0 = xMapForChipLane0(
            path[i].chip[0], path[i].chip[1]); // find x connection to sf chip

        int xMapSFc1 = xMapForNode(path[i].node2, path[i].chip[1]);
        int yMapSFc1 = path[i].chip[0];

        // if ((ch[path[i].chip[0]].xStatus[xMapBBc0] == path[i].net ||
        //      ch[path[i].chip[0]].xStatus[xMapBBc0] == -1) &&
        //     (ch[path[i].chip[1]].yStatus[yMapSFc1] == path[i].net ||
        //      ch[path[i].chip[1]].yStatus[yMapSFc1] ==
        //          -1)) // how's that for a fuckin if statement
        // {

        if (freeOrSameNetX(path[i].chip[0], xMapBBc0, path[i].net,
                           currentAllowStacking) == true &&
            freeOrSameNetY(path[i].chip[1], yMapSFc1, path[i].net,
                           currentAllowStacking) == true) {

          // Save state before making any assignments
          saveRoutingState(i);

          bool pathX0Success = setPathX(i, 0, xMapBBc0);
          bool pathX1Success = setPathX(i, 1, xMapSFc1);

          bool pathY0Success =
              setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));

          bool pathY1Success = setPathY(
              i, 1,
              path[i]
                  .chip[0]); // bb to sf connections are always in chip order,
          // so chip A is always connected to sf y 0

          bool chipX0Success = false;
          bool chipY0Success = false;
          bool chipX1Success = false;
          bool chipY1Success = false;

          if (pathX0Success) {
            chipX0Success = setChipXStatus(path[i].chip[0], xMapBBc0,
                                           path[i].net, "BBtoSF chip0");
          }
          if (pathY0Success) {
            chipY0Success = setChipYStatusSafe(path[i].chip[0], path[i].y[0],
                                               path[i].net, "BBtoSF Y0");
          }
          if (pathX1Success) {
            chipX1Success = setChipXStatus(path[i].chip[1], path[i].x[1],
                                           path[i].net, "BBtoSF chip1");
          }
          if (pathY1Success) {
            chipY1Success = setChipYStatusSafe(path[i].chip[1], path[i].chip[0],
                                               path[i].net, "BBtoSF Y1");
          }

          bool allAssignmentsSuccessful = pathX0Success && pathX1Success &&
                                          pathY0Success && pathY1Success &&
                                          chipX0Success && chipY0Success &&
                                          chipX1Success && chipY1Success;

          if (allAssignmentsSuccessful) {
            commitRoutingState();
            pathCommitted = true;
            if (debugNTCC6) {
              Serial.print("BBtoSF path[");
              Serial.print(i);
              Serial.println("] assignments committed successfully");
            }
          } else {
            restoreRoutingState(i);
            path[i].altPathNeeded = true;
            if (debugNTCC6) {
              Serial.print("BBtoSF assignment failed for path[");
              Serial.print(i);
              Serial.println("], state restored, marking for alt routing");
            }
            break;
          }

          if (debugNTCC2 == true) {
            // delay(10);

            Serial.print(" \t\n\rchip[0]: ");
            Serial.print(chipNumToChar(path[i].chip[0]));

            Serial.print("  x[0]: ");
            Serial.print(path[i].x[0]);

            Serial.print("  y[0]: ");
            Serial.print(path[i].y[0]);

            Serial.print("\t  chip[1]: ");
            Serial.print(chipNumToChar(path[i].chip[1]));

            Serial.print("  x[1]: ");
            Serial.print(path[i].x[1]);

            Serial.print("  y[1]: ");
            Serial.print(path[i].y[1]);

            Serial.print(" \t ");
            Serial.print(ch[path[i].chip[0]].xStatus[xMapBBc0]);

            Serial.print(" \t ");
            Serial.print(ch[path[i].chip[1]].xStatus[xMapSFc1]);
            Serial.print(" \t ");

            Serial.println("  ");
          }
          pathCommitted = true;
        } else {
          path[i].altPathNeeded = true;

          if (debugNTCC2) {
            // delay(10);
            Serial.print(
                "\tno direct path, setting altPathNeeded flag (BBtoSF)");
          }
          break;
        }
        break;
      }
      case NANOtoSF:
      case NANOtoNANO: {
        // Serial.print(" NANOtoNANO  ");
        int xMapNANOC0 = xMapForNode(path[i].node1, path[i].chip[0]);
        int xMapNANOC1 = xMapForNode(path[i].node2, path[i].chip[1]);

        if (debugNTCC6 && path[i].net == 23) {
          Serial.print("GP_8-D3 in commitPaths NANOtoSF path[");
          Serial.print(i);
          Serial.print("] - chip[0]=");
          Serial.print(chipNumToChar(path[i].chip[0]));
          Serial.print(" chip[1]=");
          Serial.print(chipNumToChar(path[i].chip[1]));
          Serial.print(" xMapNANOC0=");
          Serial.print(xMapNANOC0);
          Serial.print(" xMapNANOC1=");
          Serial.println(xMapNANOC1);
        }

        if (debugNTCC2) {
          Serial.print("NANOtoNANO path[");
          Serial.print(i);
          Serial.print("] - chip[0]=");
          Serial.print(chipNumToChar(path[i].chip[0]));
          Serial.print(" chip[1]=");
          Serial.print(chipNumToChar(path[i].chip[1]));
          Serial.print(" xMapNANOC0=");
          Serial.print(xMapNANOC0);
          Serial.print(" xMapNANOC1=");
          Serial.println(xMapNANOC1);
        }

        if (path[i].chip[0] == path[i].chip[1]) {
          if (debugNTCC2) {
            Serial.print("Same chip - checking X connections...");
          }
          bool x0Free = freeOrSameNetX(path[i].chip[0], xMapNANOC0, path[i].net,
                                       currentAllowStacking);
          bool x1Free = freeOrSameNetX(path[i].chip[0], xMapNANOC1, path[i].net,
                                       currentAllowStacking);
          if (debugNTCC2) {
            Serial.print(" x0Free=");
            Serial.print(x0Free);
            Serial.print(" x1Free=");
            Serial.println(x1Free);
          }

          // Check if this would violate Y position limits for high-traffic
          // chips
          bool respectsYLimits = true;
          if (path[i].chip[0] >= 8) { // Special function chips
            if (!canNetUseMoreYPositions(path[i].net)) {
              respectsYLimits = false;
              if (debugNTCC2) {
                Serial.print(" - rejected same-chip connection due to Y limits "
                             "for net ");
                Serial.println(path[i].net);
              }
            }
          }

          if (x0Free == true && respectsYLimits) {
            if (x1Free == true) {
              if (debugNTCC2) {
                Serial.print("COMMITTING NANOtoNANO same-chip path[");
                Serial.print(i);
                Serial.print("] - setting x[0]=");
                Serial.print(xMapNANOC0);
                Serial.print(" x[1]=");
                Serial.println(xMapNANOC1);
              }

              // Save state before making any assignments
              saveRoutingState(i);

              bool chipX0Success =
                  setChipXStatus(path[i].chip[0], xMapNANOC0, path[i].net,
                                 "NANOtoNANO same-chip x0");
              bool chipX1Success =
                  setChipXStatus(path[i].chip[1], xMapNANOC1, path[i].net,
                                 "NANOtoNANO same-chip x1");

              bool pathX0Success = setPathX(i, 0, xMapNANOC0);
              bool pathX1Success = setPathX(i, 1, xMapNANOC1);

              bool pathY0Success = setPathY(i, 0, -2);
              bool pathY1Success = setPathY(i, 1, -2);

              bool allAssignmentsSuccessful = chipX0Success && chipX1Success &&
                                              pathX0Success && pathX1Success &&
                                              pathY0Success && pathY1Success;

              if (allAssignmentsSuccessful) {
                commitRoutingState();
                path[i].sameChip = true;
                pathCommitted = true;
                if (debugNTCC6) {
                  Serial.print("NANOtoNANO same-chip path[");
                  Serial.print(i);
                  Serial.println("] assignments committed successfully");
                }
              } else {
                restoreRoutingState(i);
                path[i].altPathNeeded = true;
                if (debugNTCC6) {
                  Serial.print(
                      "NANOtoNANO same-chip assignment failed for path[");
                  Serial.print(i);
                  Serial.println("], state restored, marking for alt routing");
                }
              }
              // Serial.print(" ?????????");
              if (debugNTCC2) {
                Serial.print(" \t\t\tchip[0]: ");
                Serial.print(chipNumToChar(path[i].chip[0]));

                Serial.print("  x[0]: ");
                Serial.print(path[i].x[0]);

                Serial.print("  y[0]: ");
                Serial.print(path[i].y[0]);

                Serial.print("\t  chip[1]: ");
                Serial.print(chipNumToChar(path[i].chip[1]));

                Serial.print("  x[1]: ");
                Serial.print(path[i].x[1]);

                Serial.print("  y[1]: ");
                Serial.print(path[i].y[1]);
              }
              pathCommitted = true;
            } else {
              // Same chip but one X connection not available - need alt path
              path[i].altPathNeeded = true;
              if (debugNTCC2) {
                Serial.print("NANOtoNANO same chip: X1 connection conflict, "
                             "setting altPathNeeded for path ");
                Serial.println(i);
              }
            }
          } else {
            // Same chip but one X connection not available - need alt path
            path[i].altPathNeeded = true;
            if (debugNTCC2) {
              Serial.print("NANOtoNANO same chip: X0 connection conflict, "
                           "setting altPathNeeded for path ");
              Serial.println(i);
            }
          }
        } else {
          path[i].altPathNeeded = true;
          if (debugNTCC6 && path[i].net == 23) {
            Serial.print("GP_8-D3 setting altPathNeeded flag in commitPaths "
                         "(different chips: ");
            Serial.print(chipNumToChar(path[i].chip[0]));
            Serial.print(" != ");
            Serial.print(chipNumToChar(path[i].chip[1]));
            Serial.println(")");
          }
          if (debugNTCC2) {
            Serial.print(
                "\n\rno direct path, setting altPathNeeded flag (NANOtoNANO)");
            Serial.print(" \t ");
            Serial.println(i);
            printPathsCompact();
          }
        }
      }
      default:
        // Handle unhandled path types (BBtoBBL, NANOtoBBL, SFtoSF, etc.)
        break;
        // case BBtoNANO:
      }
      // if (debugNTCC2)
      // {
      //     Serial.println("\n\r");
      // }
    } // end stacking attempt loop
  }
  // duplicateSFnets();
  //    printPathsCompact();
  //     printChipStatus();

  // duplicateSFnets();
}


int ijklPaths(int pathNumber, int currentAllowStacking) {
  // return 0;
  int chip0 = path[pathNumber].chip[0];
  int chip1 = path[pathNumber].chip[1];
  // int chip2 = path[pathNumber].chip[2];
  // int chip3 = path[pathNumber].chip[3];

  if (debugNTCC6) {
    Serial.print("ijklPaths() called for path[");
    Serial.print(pathNumber);
    Serial.print("] net=");
    Serial.print(path[pathNumber].net);
    Serial.print(" chips=[");
    Serial.print(chipNumToChar(chip0));
    Serial.print(",");
    Serial.print(chipNumToChar(chip1));
    Serial.print("]");
  }

  if (chip0 == chip1) {
    if (debugNTCC6) {
      Serial.println(" - same chip, skipping");
    }
    return 0;
  }
  if (chip0 < 8 || chip1 < 8) { // allow it to find a hop here
    if (debugNTCC6) {
      Serial.println(" - involves breadboard chip, skipping");
    }
    return 0;
  }

  // Check if this path is already committed (prevent duplicates)
  if (path[pathNumber].x[0] != -1 && path[pathNumber].x[1] != -1 &&
      !path[pathNumber].altPathNeeded) {
    if (debugNTCC) {
      Serial.println("ijklPaths: path already committed, skipping");
    }
    return 1; // Path already exists and is committed
  }

  int x0 = -1;
  int x1 = -1;
  //  printPathsCompact();
  //  printChipStatus();
  for (int i = 12; i < 15; i++) {
    if (ch[chip0].xMap[i] == chip1) {
      x0 = i;
    }
    if (ch[chip1].xMap[i] == chip0) {
      x1 = i;
    }
  }
  // if ((ch[chip0].xStatus[x0] == -1 ||
  //      ch[chip0].xStatus[x0] == path[pathNumber].net) &&
  //     (ch[chip1].xStatus[x1] == -1 ||
  //      ch[chip1].xStatus[x1] == path[pathNumber].net)) {

  if (freeOrSameNetX(chip0, x0, path[pathNumber].net, currentAllowStacking) ==
          true &&
      freeOrSameNetX(chip1, x1, path[pathNumber].net, currentAllowStacking) ==
          true) {

    if (debugNTCC6) {
      Serial.print(" - SUCCESS! Direct ijkl connection established: ");
      Serial.print(chipNumToChar(chip0));
      Serial.print(".X[");
      Serial.print(x0);
      Serial.print("] <-> ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(".X[");
      Serial.print(x1);
      Serial.println("]");
    }

    // Save state before making any assignments
    saveRoutingState(pathNumber);

    bool interChipX0Success = setChipXStatus(chip0, x0, path[pathNumber].net,
                                             "ijklPaths inter-chip x0");
    bool interChipX1Success = setChipXStatus(chip1, x1, path[pathNumber].net,
                                             "ijklPaths inter-chip x1");
    bool pathX0Success =
        setPathX(pathNumber, 0, xMapForNode(path[pathNumber].node1, chip0));
    bool pathX1Success =
        setPathX(pathNumber, 1, xMapForNode(path[pathNumber].node2, chip1));

    bool nodeX0Success = true;
    bool nodeX1Success = true;
    if (path[pathNumber].x[0] != -1 && path[pathNumber].x[1] != -1) {
      nodeX0Success = setChipXStatus(chip0, path[pathNumber].x[0],
                                     path[pathNumber].net, "ijklPaths node x0");
      nodeX1Success = setChipXStatus(chip1, path[pathNumber].x[1],
                                     path[pathNumber].net, "ijklPaths node x1");
    }

    bool allAssignmentsSuccessful = interChipX0Success && interChipX1Success &&
                                    pathX0Success && pathX1Success &&
                                    nodeX0Success && nodeX1Success;

    if (!allAssignmentsSuccessful) {
      restoreRoutingState(pathNumber);
      if (debugNTCC6) {
        Serial.print("ijklPaths assignment failed for path[");
        Serial.print(pathNumber);
        Serial.println("], state restored, connection not established");
      }
      return 0; // Failed to establish connection
    }

    // If we get here, all assignments succeeded
    commitRoutingState();

    ch[path[pathNumber].chip[0]].uncommittedHops++;
    ch[path[pathNumber].chip[1]].uncommittedHops++;

    path[pathNumber].sameChip = true;
    // path[pathNumber].altPathNeeded = false;

    path[pathNumber].chip[2] = chip0;
    path[pathNumber].chip[3] = chip1;
    setPathX(pathNumber, 2, x0);
    setPathX(pathNumber, 3, x1);

    // For direct ijkl connections between special function chips, Y values
    // still need resolving Set them to -2 so resolveUncommittedHops can resolve
    // them
    setPathY(pathNumber, 0, -2);
    setPathY(pathNumber, 1, -2);
    setPathY(pathNumber, 2, -2);
    setPathY(pathNumber, 3, -2);
    //  printPathsCompact();
    //  printChipStatus();
    // Serial.print("pathNumber: ");
    // Serial.println(pathNumber);
    return 1;
  } else {
    if (debugNTCC6) {
      Serial.print(" - FAILED: ijkl connections not available (");
      Serial.print(chipNumToChar(chip0));
      Serial.print(".X[");
      Serial.print(x0);
      Serial.print("]=");
      Serial.print(ch[chip0].xStatus[x0]);
      Serial.print(", ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(".X[");
      Serial.print(x1);
      Serial.print("]=");
      Serial.print(ch[chip1].xStatus[x1]);
      Serial.println(")");
    }
    return 0;
  }
}

void resolveAltPaths(int allowStacking, int powerOnly, int noOrOnlyDuplicates) {
  // if (debugNTCC5) {
  //  Serial.println("\n\rresolveAltPaths()");
  //  Serial.println(" ");
  //  Serial.print("numberOfPaths: ");
  //  Serial.println(numberOfPaths);
  //}
  // return;
  int couldFindPath = -1;

  for (int i = 0; i < numberOfPaths; i++) {
    if (powerOnly == 1 && path[i].net > 5) {
      continue;
    }
    if (powerOnly == 1 && path[i].duplicate == 1) {
      continue;
    }

    if (noOrOnlyDuplicates == 1 && path[i].duplicate == 0) {
      continue;
    }

    if (noOrOnlyDuplicates == 0 && path[i].duplicate == 1) {
      continue;
    }
    if (path[i].altPathNeeded == true) {
      //   Serial.print("\n\n\rPATH: ");
      //   Serial.println(i);
      // Serial.print("path[i].altPathNeeded = ");
      // Serial.println(path[i].altPathNeeded);
      // Serial.print("numberOfPaths = ");
      // Serial.println(numberOfPaths);

      // Try alt path first without stacking, then with stacking if needed (only
      // if allowStacking is 2)
      bool altPathResolved = false;
      int maxStackingAttempts =
          (allowStacking == 2) ? 1
                               : 0; // 0-1 if allowStacking==2, 0-0 otherwise

      for (int stackingAttempt = 0;
           stackingAttempt <= maxStackingAttempts && !altPathResolved;
           stackingAttempt++) {
        int currentAllowStacking =
            (allowStacking == 2) ? stackingAttempt : allowStacking;

        // Reset altPathNeeded for retry attempt
        if (stackingAttempt == 1) {
          path[i].altPathNeeded = true;
        }

        // Try ijklPaths first with current stacking setting
        if (debugNTCC6 && path[i].net == 23) {
          Serial.print("Trying ijklPaths for GP_8-D3 connection (path[");
          Serial.print(i);
          Serial.println("])");
        }

        if (ijklPaths(i, currentAllowStacking) == 1) {
          if (debugNTCC6 && path[i].net == 23) {
            Serial.println("ijklPaths SUCCESS for GP_8-D3!");
          }
          altPathResolved = true;
          continue;
        }

        if (debugNTCC6 && path[i].net == 23) {
          Serial.println(
              "ijklPaths failed for GP_8-D3, trying other alt paths...");
        }

        switch (path[i].pathType) {
        case BBtoBB: {
          int foundPath = 0;
          if (debugNTCC2) {
            Serial.println("BBtoBB");
          }

          int yNode1 = yMapForNode(path[i].node1, path[i].chip[0]);
          int yNode2 = yMapForNode(path[i].node2, path[i].chip[1]);

          setChipYStatusSafe(path[i].chip[0], yNode1, path[i].net,
                             "BBtoBB alt Y0");
          setChipYStatusSafe(path[i].chip[1], yNode2, path[i].net,
                             "BBtoBB alt Y1");

          if (foundPath == 1) {
            couldFindPath = i;
            break;
          }
          int giveUpOnL = 0;
          int swapped = 0;

          int chipsWithFreeY0[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
          int numberOfChipsWithFreeY0 = 0;

          for (int chipFreeY = 0; chipFreeY < 8; chipFreeY++) {
            // if (ch[chipFreeY].yStatus[0] == -1 ||
            //     ch[chipFreeY].yStatus[0] == path[i].net) {

            if (freeOrSameNetY(chipFreeY, 0, path[i].net,
                               currentAllowStacking) == true) {
              chipsWithFreeY0[chipFreeY] = chipFreeY;
              numberOfChipsWithFreeY0++;
            }
          }

          for (int chipFreeY = 0; chipFreeY < 8; chipFreeY++) {
            if (debugNTCC2) {
              Serial.print("\n\r");
              Serial.print("path: ");
              Serial.print(i);
              Serial.print("\tindex: ");
              Serial.print(chipFreeY);
              Serial.print("  chip: ");
              printChipNumToChar(chipsWithFreeY0[chipFreeY]);
              Serial.print("\n\r");
            }
          }

          for (int bb = 0; bb < 8; bb++) // this is a long winded way to do this
                                         // but it's at least slightly readable
          {
            if (chipsWithFreeY0[bb] == -1) {
              // Serial.print("chipsWithFreeY0[");
              // Serial.print(bb);
              // Serial.print("] == -1\n\r");
              continue;
            }

            int xMapL0c0 = xMapForChipLane0(bb, path[i].chip[0]);
            int xMapL0c1 = xMapForChipLane0(bb, path[i].chip[1]);

            int xMapL1c0 = xMapForChipLane1(bb, path[i].chip[0]);
            int xMapL1c1 = xMapForChipLane1(bb, path[i].chip[1]);

            // if (bb == 7 && giveUpOnL == 0 && swapped == 0) {
            //   bb = 0;
            //   giveUpOnL = 0;
            //   swapped = 1;
            //   // Serial.println("\t\t\tt\t\t\tt\t\tswapped");
            //   // swapDuplicateNode(i);
            // } else if (bb == 7 && giveUpOnL == 0 && swapped == 1) {
            //   bb = 0;
            //   giveUpOnL = 1;
            // }

            //   if ((ch[CHIP_L].yStatus[bb] != -1 &&
            //        ch[CHIP_L].yStatus[bb] != path[i].net) &&
            //       giveUpOnL == 0) {

            //     continue;
            //   }

            if (path[i].chip[0] == bb || path[i].chip[1] == bb) {
              continue;
            }

            // if (path[i].chip[0] == path[i].chip[1]) {
            //   continue;
            // }

            // if (ch[bb].xStatus[] == path[i].net ||
            //     ch[bb].xStatus[xMapL0c0] == -1)

            if (freeOrSameNetX(bb, xMapL0c0, path[i].net,
                               currentAllowStacking) == true) {
              // were going through each bb chip to see if it has a
              // connection to both chips free

              // if (ch[bb].xStatus[xMapL0c1] == path[i].net ||
              //     ch[bb].xStatus[xMapL0c1] == -1) // lanes 0 0
              if (freeOrSameNetX(bb, xMapL0c1, path[i].net,
                                 currentAllowStacking) == true) {
                // if (ch[bb].yStatus[0] == -1) {
                setChipXStatus(bb, xMapL0c0, path[i].net,
                               "BBtoBB alt lane0 hop chip x0");
                setChipXStatus(bb, xMapL0c1, path[i].net,
                               "BBtoBB alt lane0 hop chip x1");

                //   if (giveUpOnL == 0) {
                //     ch[CHIP_L].yStatus[bb] = path[i].net;
                //     ch[bb].yStatus[0] = path[i].net;
                //     path[i].y[2] = 0;
                //     path[i].y[3] = 0;
                //   } else {
                // if (debugNTCC2) {
                //     Serial.println("Gave up on L  ");
                // Serial.println(bb);
                //                  // }

                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, path[i].net, "BBtoBB hop Y=0");
                //}

                path[i].sameChip = true;

                path[i].chip[2] = bb;
                path[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c0);
                setPathX(i, 3, xMapL0c1);

                setChipXStatus(bb, xMapL0c0, path[i].net,
                               "BBtoBB alt lane0 hop chip x0 dup");
                setChipXStatus(bb, xMapL0c1, path[i].net,
                               "BBtoBB alt lane0 hop chip x1 dup");

                // Serial.print("!!!!3 bb: ");
                // Serial.println(bb);
                // Serial.print("chip[3]: ");
                // Serial.println(path[i].chip[3]);

                path[i].altPathNeeded = false;
                altPathResolved = true;

                setPathX(i, 0, xMapForChipLane0(path[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane0(path[i].chip[1], bb));

                setChipXStatus(path[i].chip[0],
                               xMapForChipLane0(path[i].chip[0], bb),
                               path[i].net, "BBtoBB alt lane0 chip0");
                setChipXStatus(path[i].chip[1],
                               xMapForChipLane0(path[i].chip[1], bb),
                               path[i].net, "BBtoBB alt lane0 chip1");

                setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[1]));

                setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                   "BBtoBB alt lane0 y0");
                setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                   "BBtoBB alt lane0 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(path[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(path[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(path[i].x[3]);

                  Serial.print(" \n\r");
                }
                // printPathsCompact();
                //}
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL1c0, path[i].net,
                               currentAllowStacking) == true) {
              if (freeOrSameNetX(bb, xMapL1c1, path[i].net,
                                 currentAllowStacking) == true) {
                setChipXStatus(bb, xMapL1c0, path[i].net,
                               "BBtoBB alt lane1 hop x0");
                setChipXStatus(bb, xMapL1c1, path[i].net,
                               "BBtoBB alt lane1 hop x1");

                //   if (giveUpOnL == 0) {
                //     // Serial.print("Give up on L?");
                //     ch[CHIP_L].yStatus[bb] = path[i].net;
                //     ch[bb].yStatus[0] = path[i].net;
                //     path[i].y[2] = 0;
                //     path[i].y[3] = 0;
                //   } else {
                // if (debugNTCC2) {
                // Serial.println("Gave up on L");
                //}
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, path[i].net,
                                   "BBtoBB alt lane1 hop y0");
                /// }

                path[i].chip[2] = bb;
                path[i].chip[3] = bb;
                setPathX(i, 2, xMapL1c0);
                setPathX(i, 3, xMapL1c1);
                path[i].sameChip = true;
                path[i].altPathNeeded = false;
                altPathResolved = true;

                setChipXStatus(bb, xMapL1c0, path[i].net,
                               "BBtoBB alt lane1 hop x0 dup");
                setChipXStatus(bb, xMapL1c1, path[i].net,
                               "BBtoBB alt lane1 hop x1 dup");

                setPathX(i, 0, xMapForChipLane1(path[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane1(path[i].chip[1], bb));

                setChipXStatus(path[i].chip[0],
                               xMapForChipLane1(path[i].chip[0], bb),
                               path[i].net, "BBtoBB alt lane1 chip0");
                setChipXStatus(path[i].chip[1],
                               xMapForChipLane1(path[i].chip[1], bb),
                               path[i].net, "BBtoBB alt lane1 chip1");

                setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[1]));

                setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                   "BBtoBB alt lane1 y0");
                setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                   "BBtoBB alt lane1 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(path[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(path[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(path[i].x[3]);

                  Serial.print(" \n\r");
                }
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL0c0, path[i].net,
                               currentAllowStacking) == true &&
                false) {
              if (freeOrSameNetX(bb, xMapL1c1, path[i].net,
                                 currentAllowStacking) == true) {
                //   if (giveUpOnL == 0) {
                //     ch[CHIP_L].yStatus[bb] = path[i].net;
                //     ch[bb].yStatus[0] = path[i].net;
                //     path[i].y[2] = 0;
                //     path[i].y[3] = 0;
                //   } else {
                if (debugNTCC2) {
                  Serial.println("Gave up on L");
                }
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, path[i].net,
                                   "BBtoBB alt mixed hop y0");
                //}

                setChipXStatus(bb, xMapL0c0, path[i].net,
                               "BBtoBB alt mixed hop x0");
                setChipXStatus(bb, xMapL1c1, path[i].net,
                               "BBtoBB alt mixed hop x1");

                path[i].chip[2] = bb;
                path[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c0);
                setPathX(i, 3, xMapL1c1);

                path[i].altPathNeeded = false;
                altPathResolved = true;

                setPathX(i, 0, xMapForChipLane0(path[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane1(path[i].chip[1], bb));

                setChipXStatus(path[i].chip[0],
                               xMapForChipLane0(path[i].chip[0], bb),
                               path[i].net, "BBtoBB alt mixed chip0");
                setChipXStatus(path[i].chip[1],
                               xMapForChipLane1(path[i].chip[1], bb),
                               path[i].net, "BBtoBB alt mixed chip1");

                setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[1]));

                setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                   "BBtoBB alt mixed y0");
                setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                   "BBtoBB alt mixed y1");

                path[i].sameChip = true;
                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(path[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(path[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(path[i].x[3]);

                  Serial.print(" \n\r");
                }
                // printPathsCompact();
                //}
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL1c0, path[i].net,
                               currentAllowStacking) == true &&
                false) {
              if (freeOrSameNetX(bb, xMapL0c1, path[i].net,
                                 currentAllowStacking) == true) {
                //   if (giveUpOnL == 0) {
                //     ch[CHIP_L].yStatus[bb] = path[i].net;
                //     ch[bb].yStatus[0] = path[i].net;
                //     path[i].y[2] = 0;
                //     path[i].y[3] = 0;
                //   } else {
                //     if (debugNTCC2) {
                //       Serial.println("Gave up on L");
                //     }
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, path[i].net,
                                   "BBtoBB alt mixed2 hop y0");
                //}

                setChipXStatus(bb, xMapL0c1, path[i].net,
                               "BBtoBB alt mixed2 hop x0");
                setChipXStatus(bb, xMapL1c0, path[i].net,
                               "BBtoBB alt mixed2 hop x1");

                path[i].chip[2] = bb;
                path[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c1);
                setPathX(i, 3, xMapL1c0);
                // path[i].y[2] = -2;
                // path[i].y[3] = -2;
                path[i].altPathNeeded = false;
                altPathResolved = true;
                path[i].sameChip = true;
                setPathX(i, 0, xMapForChipLane1(path[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane0(path[i].chip[1], bb));

                setChipXStatus(path[i].chip[0],
                               xMapForChipLane1(path[i].chip[0], bb),
                               path[i].net, "BBtoBB alt mixed2 chip0");
                setChipXStatus(path[i].chip[1],
                               xMapForChipLane0(path[i].chip[1], bb),
                               path[i].net, "BBtoBB alt mixed2 chip1");

                setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                setPathY(i, 1, yMapForNode(path[i].node2, path[i].chip[1]));

                setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                   "BBtoBB alt mixed2 y0");
                setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                   "BBtoBB alt mixed2 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print(" chip[2]: ");
                  Serial.print(chipNumToChar(path[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(path[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(path[i].x[3]);

                  Serial.print(" \n\r");
                }
                couldFindPath = i;
                // continue;
                break;
              }

              if (debugNTCC2) {
                Serial.print("\n\r");
                Serial.print(i);
                Serial.print("  chip[2]: ");
                Serial.print(chipNumToChar(path[i].chip[2]));

                Serial.print("  x[2]: ");
                Serial.print(path[i].x[2]);

                Serial.print("  x[3]: ");
                Serial.print(path[i].x[3]);

                Serial.print(" \n\r");
              }
            }
            //}
          }
          // continue;
          break;
        }

        case NANOtoSF:
        case NANOtoNANO: {
          if (debugNTCC2) {
            Serial.println("   NANOtoNANO");
          }
          int foundHop = 0;
          int giveUpOnL = 0;
          int swapped = 0;
          // duplicateSFnets();
          ////printPathsCompact();
          // printChipStatus();
          giveUpOnL = 0;

          // Serial.print(i);
          // Serial.println("   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

          for (int bb = 0; bb < 8; bb++) // this is a long winded way to do this
                                         // but it's at least slightly readable
          {
            int sfChip1 = path[i].chip[0];
            int sfChip2 = path[i].chip[1];

            int chip1Lane = xMapForNode(sfChip1, bb);
            int chip2Lane = xMapForNode(sfChip2, bb);

            if (bb == 7 && giveUpOnL == 0 && swapped == 0) {
              bb = 0;
              giveUpOnL = 0;
              swapped = 1;
              // swapDuplicateNode(i);
            } else if (bb == 7 && giveUpOnL == 0 && swapped == 1) {
              // bb = 0;
              giveUpOnL = 1;
            }

            // if ((ch[CHIP_L].yStatus[bb] != -1 &&
            //      ch[CHIP_L].yStatus[bb] != path[i].net) &&
            //     giveUpOnL == 0) {

            //   continue;
            // }
            // if ((ch[path[i].chip[0]].yStatus[bb] != -1 &&
            //      ch[path[i].chip[0]].yStatus[bb] != path[i].net) ||
            //     (ch[path[i].chip[1]].yStatus[bb] != -1 &&
            //      ch[path[i].chip[1]].yStatus[bb] != path[i].net) ||
            //     (ch[bb].xStatus[chip1Lane] != -1 &&
            //      ch[bb].xStatus[chip1Lane] != path[i].net) ||
            //     (ch[bb].xStatus[chip2Lane] != -1 &&
            //      ch[bb].xStatus[chip2Lane] != path[i].net) ||
            //     (ch[bb].yStatus[0] != -1 && ch[bb].yStatus[0] !=
            //     path[i].net))
            //     {

            if (freeOrSameNetY(path[i].chip[0], bb, path[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetY(path[i].chip[1], bb, path[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetX(bb, chip1Lane, path[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetX(bb, chip2Lane, path[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetY(bb, 0, path[i].net, currentAllowStacking) ==
                    false) {
              //                 Serial.print("bb:\t");
              // Serial.println(bb);
              // Serial.print("xStatus:\t");
              // Serial.println(ch[bb].xStatus[chip1Lane]);
              // Serial.print("xStatus:\t");
              // Serial.println(ch[bb].xStatus[chip2Lane]);
              // Serial.println(" ");
              // Serial.print("path: ");
              // Serial.println(i);
              //    printPathsCompact();
              //   printChipStatus();
              //   Serial.print("!!!!!!!!!!!!!!!!!!!!!!!!\n\r");
              // continue;
            } else {
              // if (ch[bb].xStatus[chip1Lane] == path[i].net ||
              //     ch[bb].xStatus[chip1Lane] == -1) {
              if (freeOrSameNetX(bb, chip1Lane, path[i].net,
                                 currentAllowStacking) == true) {
                // if (ch[bb].xStatus[chip2Lane] == path[i].net ||
                //     ch[bb].xStatus[chip2Lane] == -1) {
                if (freeOrSameNetX(bb, chip2Lane, path[i].net,
                                   currentAllowStacking) == true) {
                  // printPathsCompact();
                  // printChipStatus();

                  if (giveUpOnL == 1) {
                    if (debugNTCC2) {
                      // Serial.println("Gave up on L");
                      // Serial.print("path :");
                      // Serial.println(i);
                    }
                    continue;
                    // break;
                  }

                  path[i].sameChip = true;

                  setChipXStatus(bb, chip1Lane, path[i].net,
                                 "NANOtoNANO alt hop x0");
                  setChipXStatus(bb, chip2Lane, path[i].net,
                                 "NANOtoNANO alt hop x1");

                  if (path[i].chip[0] != path[i].chip[1]) {
                    //                 Serial.println("VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV");
                    //                 int pathAddress = (int)&path[i];
                    // Serial.print(pathAddress);
                    // Serial.print("\tpath: ");
                    // Serial.println(i);
                    // Serial.print("bb:\t");
                    // Serial.print(bb);

                    path[i].chip[2] = bb;
                    path[i].chip[3] = bb;
                    setPathY(i, 2, 0);
                    setPathY(i, 3, 0);
                    setChipYStatusSafe(bb, 0, path[i].net,
                                       "NANOtoNANO alt hop y0");

                    setPathX(i, 2, chip1Lane);
                    setPathX(i, 3, chip2Lane);
                  }

                  path[i].altPathNeeded = false;
                  altPathResolved = true;

                  setPathX(i, 0, xMapForNode(path[i].node1, path[i].chip[0]));
                  setPathX(i, 1, xMapForNode(path[i].node2, path[i].chip[1]));
                  setChipXStatus(path[i].chip[0],
                                 xMapForNode(path[i].node1, path[i].chip[0]),
                                 path[i].net, "NANOtoNANO alt chip0");
                  setChipXStatus(path[i].chip[1],
                                 xMapForNode(path[i].node2, path[i].chip[1]),
                                 path[i].net, "NANOtoNANO alt chip1");

                  setPathY(i, 0, bb);
                  setPathY(i, 1, bb);

                  //            Serial.print(">>>> path ");
                  // Serial.println(i);

                  setChipYStatusSafe(path[i].chip[0], bb, path[i].net,
                                     "NANOtoNANO alt y0");
                  setChipYStatusSafe(path[i].chip[1], bb, path[i].net,
                                     "NANOtoNANO alt y1");

                  if (debugNTCC2) {
                    Serial.println("\n\r");
                    Serial.print(i);
                    Serial.print("  chip[2]: ");
                    Serial.print(chipNumToChar(path[i].chip[2]));

                    Serial.print("  y[2]: ");
                    Serial.print(path[i].y[2]);

                    Serial.print("  y[3]: ");
                    Serial.print(path[i].y[3]);

                    Serial.println(" \n\r");
                  }

                  foundHop = 1;
                  couldFindPath = i;
                  // printPathsCompact();
                  // printChipStatus();
                  // Serial.println("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
                  //  printPathsCompact();
                  //  printChipStatus();

                  // Serial.print("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
                  // continue;
                  break;
                }
              }
            }
          }

          for (int bb = 0; bb < 8;
               bb++) // this will connect to a random breadboard row, add a
          // test to make sure nothing is connected
          {
            int sfChip1 = path[i].chip[0];
            int sfChip2 = path[i].chip[1];

            int chip1Lane = xMapForNode(sfChip1, bb);
            int chip2Lane = xMapForNode(sfChip2, bb);

            // Serial.print("bb:\t");
            // Serial.println(bb);
            // Serial.print("xStatus:\t");
            // Serial.println(ch[bb].xStatus[chip1Lane]);
            // Serial.print("xStatus:\t");
            // Serial.println(ch[bb].xStatus[chip2Lane]);
            // Serial.println(" ");
            // Serial.print("path: ");
            // Serial.println(i);
            // if ((ch[path[i].chip[0]].yStatus[bb] != -1 &&
            //      ch[path[i].chip[0]].yStatus[bb] != path[i].net) ||
            //     (ch[path[i].chip[1]].yStatus[bb] != -1 &&
            //      ch[path[i].chip[1]].yStatus[bb] != path[i].net) ||
            //     (ch[bb].xStatus[chip1Lane] != -1 &&
            //      ch[bb].xStatus[chip1Lane] != path[i].net) ||
            //     (ch[bb].xStatus[chip2Lane] != -1 &&
            //      ch[bb].xStatus[chip2Lane] != path[i].net) ||
            //     (ch[bb].yStatus[0] != -1 && ch[bb].yStatus[0] !=
            //     path[i].net))
            //     {

            if (freeOrSameNetY(path[i].chip[0], bb, path[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetY(path[i].chip[1], bb, path[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetX(bb, chip1Lane, path[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetX(bb, chip2Lane, path[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetY(bb, 0, path[i].net, currentAllowStacking) ==
                    true) {
              //                 Serial.print("bb:\t");
              // Serial.println(bb);
              // Serial.print("xStatus:\t");
              // Serial.println(ch[bb].xStatus[chip1Lane]);
              // Serial.print("xStatus:\t");
              // Serial.println(ch[bb].xStatus[chip2Lane]);
              // Serial.println(" ");
              // Serial.print("path: ");
              // Serial.println(i);
              //    printPathsCompact();
              //   printChipStatus();
              //   Serial.print("?????????????????????\n\r");
              //   // continue;
            } else {
              // Serial.print("?????????????????????\n\r");
              // if ((ch[bb].xStatus[chip1Lane] == path[i].net ||
              //      ch[bb].xStatus[chip1Lane] == -1) &&
              //     foundHop == 0) {
              if (freeOrSameNetX(bb, chip1Lane, path[i].net,
                                 currentAllowStacking) == true &&
                  foundHop == 0) {
                if (freeOrSameNetX(bb, chip2Lane, path[i].net,
                                   currentAllowStacking) == true) {
                  // Serial.print("path :");
                  // Serial.println(i);
                  //  printPathsCompact();
                  setChipXStatus(bb, chip1Lane, path[i].net,
                                 "NANOtoNANO random bb hop x0");
                  setChipXStatus(bb, chip2Lane, path[i].net,
                                 "NANOtoNANO random bb hop x1");
                  setChipYStatusSafe(bb, 0, path[i].net,
                                     "NANOtoNANO random bb hop y0");
                  if (path[i].chip[0] !=
                      path[i].chip[1]) // this makes it not try to find a third
                  // chip if it doesn't need to
                  {
                    path[i].chip[2] = bb;
                    setPathX(i, 2, chip1Lane);
                    setPathX(i, 3, chip2Lane);

                    setPathY(i, 2, 0);
                    setPathY(i, 3, 0);
                  }

                  path[i].sameChip = true;
                  path[i].altPathNeeded = false;
                  altPathResolved = true;

                  setPathX(i, 0, xMapForNode(path[i].node1, path[i].chip[0]));
                  setPathX(i, 1, xMapForNode(path[i].node2, path[i].chip[1]));
                  setChipXStatus(path[i].chip[0],
                                 xMapForNode(path[i].node1, path[i].chip[0]),
                                 path[i].net, "NANOtoNANO random bb chip0");
                  setChipXStatus(path[i].chip[1],
                                 xMapForNode(path[i].node2, path[i].chip[1]),
                                 path[i].net, "NANOtoNANO random bb chip1");
                  // Serial.print(">>>> path ");
                  // Serial.println(i);

                  setPathY(i, 0, bb);
                  setPathY(i, 1, bb);
                  setChipYStatusSafe(path[i].chip[0], bb, path[i].net,
                                     "NANOtoNANO random bb y0");
                  setChipYStatusSafe(path[i].chip[1], bb, path[i].net,
                                     "NANOtoNANO random bb y1");

                  if (debugNTCC2) {
                    Serial.print("\n\r");
                    Serial.print(i);
                    Serial.print("  chip[2]: ");
                    Serial.print(chipNumToChar(path[i].chip[2]));
                    // Serial.print("  y[2]: ");

                    Serial.print("  y[2]: ");
                    Serial.print(path[i].y[2]);

                    Serial.print("  y[3]: ");
                    Serial.print(path[i].y[3]);

                    Serial.print(" \n\r");
                  }
                  foundHop = 1;
                  couldFindPath = i;
                  // printPathsCompact();
                  // printChipStatus();
                  // continue;
                  // printPathsCompact();
                  // printChipStatus();
                  break;
                }
              }
            }
          }
          // couldntFindPath(i);
        }
        case BBtoSF: {
          // Serial.print("path[");
          // Serial.print(i);
          // Serial.println("] ");

          // duplicateSFnets();
          if (path[i].pathType == BBtoSF ||
              path[i].pathType == BBtoNANO) // do bb to sf first because these
          // are hardest to find
          {
            int foundPath = 0;

            if (debugNTCC2) {
              Serial.print("\n\rBBtoSF\tpath: ");
              Serial.println(i);
            }

            int saveUncommittedHops = ch[path[i].chip[0]].uncommittedHops;
            int saveUncommittedHops1 = ch[path[i].chip[1]].uncommittedHops;
            // Serial.print("saveUncommittedHops1: ");
            // Serial.println(saveUncommittedHops1);

            // Serial.print("saveUncommittedHops: ");
            // Serial.println(saveUncommittedHops);

            for (int bb = 0; bb < (8 - saveUncommittedHops1);
                 bb++) // check if any other chips have free paths to both the
            // sf chip and target chip
            {
              // tryAfterSwap:

              if (foundPath == 1) {
                if (debugNTCC2) {
                  Serial.print("Found Path!\n\r");
                }
                couldFindPath = i;

                break;
              }

              int xMapBB = xMapForChipLane0(path[i].chip[0], bb);
              if (xMapBB == -1) {
                // Serial.print("xMapBB == -1");

                continue; // don't bother checking if there's no connection
              }
              // if (xMapForChipLane1(path[i].chip[0], bb) == -1)
              // {
              //     //Serial.print("xMapForChipLane1(path[i].chip[0], bb) !=
              //     -1");

              //     continue; // don't bother checking if there's no connection
              // }
              // Serial.print("           fuck         ");
              int yMapSF = bb; // always

              int sfChip = path[i].chip[1];

              // not chip L
              if (debugNTCC2) {
                Serial.print("\n\r");
                Serial.print("      bb: ");
                printChipNumToChar(bb);
                Serial.print("  \t  sfChip: ");
                printChipNumToChar(sfChip);
                Serial.print("  \t  xMapBB: ");
                Serial.print(xMapBB);
                Serial.print("  \t  yMapSF: ");
                Serial.print(yMapSF);
                Serial.print("  \t  xStatus: ");
                Serial.print(ch[bb].xStatus[xMapBB]);
                Serial.print("  \n\r");
              }

              if (freeOrSameNetX(bb, xMapBB, path[i].net,
                                 currentAllowStacking) == true &&
                  freeOrSameNetY(bb, 0, path[i].net, currentAllowStacking) ==
                      true) {
                // were going through each bb chip to see if it has a
                // connection to both chips free

                int xMapL0c0 = xMapForChipLane0(path[i].chip[0], bb);
                int xMapL1c0 = xMapForChipLane1(path[i].chip[0], bb);

                int xMapL0c1 = xMapForChipLane0(bb, path[i].chip[0]);
                int xMapL1c1 = xMapForChipLane1(bb, path[i].chip[0]);

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print("      bb: ");
                  printChipNumToChar(bb);
                  Serial.print("  \t  sfChip: ");
                  printChipNumToChar(sfChip);
                  Serial.print("  \t  xMapBB: ");
                  Serial.print(xMapBB);
                  Serial.print("  \t  yMapSF: ");
                  Serial.print(yMapSF);
                  Serial.print("  \t  xStatus: ");
                  Serial.print(ch[bb].xStatus[xMapBB]);
                  Serial.print("  \n\r");

                  Serial.print("xMapL0c0: ");
                  Serial.print(xMapL0c0);
                  Serial.print("  \txMapL1c0: ");

                  Serial.print(xMapL0c1);
                  Serial.print("  \txMapL1c1: ");

                  Serial.print(xMapL1c0);
                  Serial.print("  \txMapL0c1: ");
                  Serial.print(xMapL1c1);
                  Serial.print("\n\n\r");
                }
                int freeLane = -1;
                // Serial.print("\t");
                // Serial.print(bb);

                // if ((xMapL1c0 != -1) &&
                //     ch[path[i].chip[0]].xStatus[xMapL1c0] ==
                //         path[i].net) // check if lane 1 shares a net first so
                //         it
                //                      // should prefer sharing lanes
                // {
                //   freeLane = 1;
                //} else

                // if ((ch[path[i].chip[0]].xStatus[xMapL0c0] == -1) ||
                //            ch[path[i].chip[0]].xStatus[xMapL0c0] ==
                //                path[i].net) // lanes will alway be taken
                //                together,
                //                             // so only check chip 1
                // {
                if (freeOrSameNetX(path[i].chip[0], xMapL0c0, path[i].net,
                                   currentAllowStacking) == true) {
                  freeLane = 0;
                  // } else if ((xMapL1c0 != -1) &&
                  //            ((ch[path[i].chip[0]].xStatus[xMapL1c0] == -1)
                  //            ||
                  //             ch[path[i].chip[0]].xStatus[xMapL1c0] ==
                  //             path[i].net)) {
                } else if (freeOrSameNetX(path[i].chip[0], xMapL1c0,
                                          path[i].net,
                                          currentAllowStacking) == true) {
                  freeLane = 1;
                } else {
                  continue;
                }

                // if (ch[sfChip].yStatus[yMapSF] != -1 &&
                //     ch[sfChip].yStatus[yMapSF] != path[i].net) {
                if (freeOrSameNetY(sfChip, yMapSF, path[i].net,
                                   currentAllowStacking) == false) {
                  continue;
                }

                path[i].altPathNeeded = false;
                altPathResolved = true;

                int SFnode = xMapForNode(path[i].node2, path[i].chip[1]);
                // Serial.print("\n\r\t\t\t\tSFnode: ");
                // Serial.println(SFnode);
                // Serial.print("\n\r\t\t\t\tFree Lane: ");
                // Serial.println(freeLane);

                if (freeLane == 0) {
                  path[i].chip[2] = bb;
                  path[i].chip[3] = bb;
                  setChipXStatus(path[i].chip[0], xMapL0c0, path[i].net,
                                 "BBtoSF alt lane0 chip0");
                  setChipXStatus(path[i].chip[1], SFnode, path[i].net,
                                 "BBtoSF alt lane0 chip1");

                  setChipXStatus(path[i].chip[2], xMapL0c1, path[i].net,
                                 "BBtoSF alt lane0 hop x0");
                  setChipXStatus(path[i].chip[2], xMapBB, path[i].net,
                                 "BBtoSF alt lane0 hop x1");

                  setPathX(i, 0, xMapL0c0);
                  setPathX(i, 1, SFnode);

                  setPathX(i, 2, xMapL0c1);
                  // Serial.print("\n\r\t\t\t\txBB: ");
                  // Serial.println(bb);

                  xMapBB = xMapForChipLane0(path[i].chip[2], path[i].chip[1]);
                  // Serial.println(xMapBB);
                  path[i].chip[3] = path[i].chip[2];

                  setPathX(i, 3, xMapBB);

                  setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                  setPathY(i, 1, yMapSF);
                  setPathY(i, 2, 0);
                  setPathY(i, 3, 0);
                  setChipYStatusSafe(bb, 0, path[i].net,
                                     "BBtoSF alt lane0 bb y0");

                  setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                     "BBtoSF alt lane0 chip0 y0");

                  setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                     "BBtoSF alt lane0 chip1 y1");
                  setChipYStatusSafe(path[i].chip[2], 0, path[i].net,
                                     "BBtoSF alt lane0 hop y0");

                  path[i].sameChip = true;
                } else if (freeLane == 1) {
                  path[i].chip[2] = bb;
                  path[i].chip[3] = bb;
                  setChipXStatus(path[i].chip[0], xMapL1c0, path[i].net,
                                 "BBtoSF alt lane1 chip0");
                  setChipXStatus(path[i].chip[1], SFnode, path[i].net,
                                 "BBtoSF alt lane1 chip1");

                  setChipXStatus(path[i].chip[2], xMapL1c1, path[i].net,
                                 "BBtoSF alt lane1 hop x0");
                  setChipXStatus(path[i].chip[2], xMapBB, path[i].net,
                                 "BBtoSF alt lane1 hop x1");

                  setPathX(i, 0, xMapL1c0);
                  setPathX(i, 1, SFnode);

                  setPathX(i, 2, xMapL1c1);
                  xMapBB = xMapForChipLane0(path[i].chip[2], path[i].chip[1]);
                  setPathX(i, 3, xMapBB);

                  path[i].chip[3] = path[i].chip[2];

                  setPathY(i, 0, yMapForNode(path[i].node1, path[i].chip[0]));
                  setPathY(i, 1, yMapSF);
                  setPathY(i, 2, 0);
                  setPathY(i, 3, 0);
                  setChipYStatusSafe(bb, 0, path[i].net,
                                     "BBtoSF alt lane1 bb y0");

                  setChipYStatusSafe(path[i].chip[0], path[i].y[0], path[i].net,
                                     "BBtoSF alt lane1 chip0 y0");

                  setChipYStatusSafe(path[i].chip[1], path[i].y[1], path[i].net,
                                     "BBtoSF alt lane1 chip1 y1");
                  setChipYStatusSafe(path[i].chip[2], 0, path[i].net,
                                     "BBtoSF alt lane1 hop y0");
                }

                foundPath = 1;
                couldFindPath = i;

                if (debugNTCC2 == true) {
                  Serial.print("\n\rFreelane = ");
                  Serial.print(freeLane);
                  Serial.print("\t path: ");
                  Serial.print(i);
                  Serial.print(" \n\rchip[0]: ");
                  Serial.print(chipNumToChar(path[i].chip[0]));

                  Serial.print("  x[0]: ");
                  Serial.print(path[i].x[0]);

                  Serial.print("  y[0]: ");
                  Serial.print(path[i].y[0]);

                  Serial.print("\t  chip[1]: ");
                  Serial.print(chipNumToChar(path[i].chip[1]));

                  Serial.print("  x[1]: ");
                  Serial.print(path[i].x[1]);

                  Serial.print("  y[1]: ");
                  Serial.print(path[i].y[1]);

                  Serial.print("   ch[path[i].chip[0]].xStatus[");
                  Serial.print(xMapL0c0);
                  Serial.print("]: ");
                  Serial.print(ch[path[i].chip[0]].xStatus[xMapL0c0]);

                  Serial.print("   ch[path[i].chip[1]].xStatus[ ");
                  Serial.print(xMapL0c1);
                  Serial.print("]: ");
                  Serial.print(ch[path[i].chip[1]].xStatus[xMapL0c1]);
                  Serial.print(" \t\n\r");
                  // printChipStatus();
                }
                // break;
              }

              // if (foundPath == 0 && swapped == 0 && bb == 7) {
              //   //swapped = 1;
              //   // if (debugNTCC2 == true)
              //   //  Serial.print("\n\rtrying again with swapped nodes\n\r");

              //   // path[i].x[0] = xMapForNode(path[i].node2,
              //   path[i].chip[0]);
              //   // swapDuplicateNode(i);
              //   bb = 0;
              //   // goto tryAfterSwap;
              // }
            }
          }
          break;
        }

        default:
          // Handle unhandled path types (BBtoBBL, NANOtoBBL, SFtoSF, etc.)
          break;
          // break;
        } // end stacking attempt loop
      }
    }
  }
}

bool freeOrSameNetX(int chip, int x, int net, int allowStacking) {
  // Serial.print("freeOrSameNetX: ");
  // Serial.print(chip);
  // Serial.print(", ");
  // Serial.print(x);
  // Serial.print(", ");
  // Serial.print(net);
  // Serial.print(", ");
  // Serial.print(allowStacking);
  // Serial.print(" = ");
  if (ch[chip].xStatus[x] == -1 ||
      (ch[chip].xStatus[x] == net && allowStacking == 1)) {
    // Serial.println("true");
    return true;
  } else {
    // Serial.println("false");
    return false;
  }
}

bool freeOrSameNetY(int chip, int y, int net, int allowStacking) {
  // Serial.print("freeOrSameNetY: ");
  // Serial.print(chip);
  // Serial.print(", ");
  // Serial.print(y);
  // Serial.print(", ");
  // Serial.print(net);
  // Serial.print(", ");
  // Serial.print(allowStacking);
  // Serial.print(" = ");
  if (ch[chip].yStatus[y] == -1 ||
      (ch[chip].yStatus[y] == net && allowStacking == 1)) {
    // Serial.println("true");
    return true;
  } else {
    // Serial.println("false");
    return false;
  }
}

bool frontEnd(int chip, int y, int x) { // is this an externally facing node
  if (chip < 8) {
    if (y == 0) // bounce node
    {
      return false;
    } else {
      return true;
    }
  }
  if (chip >= 8) {
    if (x >= 12 && x <= 14) // ijkl
      return false;
  } else {
    return true;
  }

  return false;
}

// Serial.print("path");
// Serial.print(i);

// resolveUncommittedHops();

// printPathsCompact();
// printChipStatus();

void couldntFindPath(int forcePrint) {
  if (debugNTCC2 == true || forcePrint == 1 || debugNTCC5 == true) {
    // Serial.print("\n\r");
  }
  numberOfUnconnectablePaths = 0;
  for (int i = 0; i < numberOfPaths; i++) {
    if (debugNTCC5) {
      Serial.print("path ");
      Serial.println(i);
    }
    int foundNegative = 0;
    for (int j = 0; j < 3; j++) {
      if (path[i].chip[j] == -1 && j >= 2) {
        continue;
      }

      if (path[i].x[j] < 0 || path[i].y[j] < 0) {
        foundNegative = 1;
      }
    }

    if (foundNegative == 1 && path[i].duplicate == 0) {

      if (debugNTCC2 == true || forcePrint == 1) {
        Serial.print("\n\rCouldn't find a path for ");
        printNodeOrName(path[i].node1);
        Serial.print(" to ");
        printNodeOrName(path[i].node2);
        Serial.println("\n\r");
      }

      unconnectablePaths[numberOfUnconnectablePaths][0] = path[i].node1;
      unconnectablePaths[numberOfUnconnectablePaths][1] = path[i].node2;
      numberOfUnconnectablePaths++;
      path[i].skip = true;
    }
  }
  if (debugNTCC2 == true || forcePrint == 1 || debugNTCC5 == true) {
    // Serial.print("\n\r");
  }
}

void resolveUncommittedHops2(void) {}
  const int freeXSearchOrder[12][16] = {
      // this disallows bounces from sf x pins that would cause problems (5V, GND, etc.)
      {-1, -1, 2, 3, 4, 5, 6, 7, 8, -1, 10, 11, 12, -1, 14, 15},        // a
      {0, 1, -1, -1, 4, 5, 6, 7, 8, 9, 10, -1, 12, 13, 14, -1},         // b
      {0, 1, 2, 3, -1, -1, 6, 7, 8, -1, 10, 11, 12, -1, 14, 15},        // c
      {0, 1, 2, 3, 4, 5, -1, -1, 8, 9, 10, -1, 12, 13, 14, -1},         // d
      {0, -1, 2, 3, 4, -1, 6, 7, -1, -1, 10, 11, 12, 13, 14, 15},       // e
      {0, 1, 2, -1, 4, 5, 6, -1, 8, 9, -1, -1, 12, 13, 14, 15},         // f
      {0, -1, 2, 3, 4, -1, 6, 7, 8, 9, 10, 11, -1, -1, 14, 15},         // g
      {0, 1, 2, -1, 4, 5, 6, -1, 8, 9, 10, 11, 12, 13, -1, -1},         // h
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 11, 12, 13, 14, -1}, // i
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // j
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // k
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // l
  };

void resolveUncommittedHops(int allowStacking, int powerOnly,
                            int noOrOnlyDuplicates) {
  if (debugNTCC2) {
    Serial.println("\nresolveUncommittedHops()");
  }

  for (int i = 0; i < numberOfPaths; i++) {
    // Filter paths based on powerOnly and duplicates flags
    if (powerOnly == 1 && (path[i].net > 5 || path[i].duplicate == 1)) {
      continue;
    }
    if (noOrOnlyDuplicates == 1 && path[i].duplicate == 0) {
      continue;
    }
    if (noOrOnlyDuplicates == 0 && path[i].duplicate == 1) {
      continue;
    }

    // Check if this path needs resolution
    bool hasUnresolvedY = false;
    bool hasUnresolvedX = false;

    for (int checkY = 0; checkY < 4; checkY++) {
      if (path[i].y[checkY] == -2) {
        hasUnresolvedY = true;
        break;
      }
    }

    for (int checkX = 0; checkX < 4; checkX++) {
      if (path[i].x[checkX] == -2) {
        hasUnresolvedX = true;
        break;
      }
    }

    // Skip paths that don't need resolution
    if (!hasUnresolvedY && !hasUnresolvedX) {
      continue;
    }

    if (debugNTCC2) {
      Serial.print("Path[");
      Serial.print(i);
      Serial.print("] net=");
      Serial.print(path[i].net);
      Serial.print(" has unresolved positions: ");
      if (hasUnresolvedY) Serial.print("Y ");
      if (hasUnresolvedX) Serial.print("X ");
      Serial.println();
    }

    // Save state before making changes
    saveRoutingState(i);
    bool allAssignmentsSucceeded = true;

    // Handle X position assignments
    if (hasUnresolvedX) {
      // For same-chip connections, find one X position that works for both nodes
      if (path[i].sameChip && path[i].chip[0] == path[i].chip[1] && 
          path[i].x[0] == -2 && path[i].x[1] == -2) {
        
        int targetChip = path[i].chip[0];
        int sharedX = -1;
        
        // Find a free X position that can be used for both nodes on the same chip
        for (int searchIdx = 0; searchIdx < 16; searchIdx++) {
          if (freeXSearchOrder[targetChip][searchIdx] == -1) {
            continue;
          }
          
          int testX = freeXSearchOrder[targetChip][searchIdx];
          if (freeOrSameNetX(targetChip, testX, path[i].net, allowStacking)) {
            sharedX = testX;
            break;
          }
        }
        
        if (sharedX != -1) {
          // Assign the same X position to both positions for same-chip connection
          bool success = setPathX(i, 0, sharedX) && setPathX(i, 1, sharedX);
          bool chipSuccess = false;
          if (success) {
            chipSuccess = setChipXStatus(targetChip, sharedX, path[i].net, "resolveUncommittedHops same-chip X");
          }
          
          if (!success || !chipSuccess) {
            allAssignmentsSucceeded = false;
          } else {
            if (debugNTCC2) {
              Serial.print("  Assigned shared X=");
              Serial.print(sharedX);
              Serial.print(" to same-chip path[");
              Serial.print(i);
              Serial.print("] on chip ");
              Serial.println(chipNumToChar(targetChip));
            }
          }
        } else {
          allAssignmentsSucceeded = false;
          if (debugNTCC2) {
            Serial.print("ERROR: No free shared X found for same-chip path[");
            Serial.print(i);
            Serial.println("]");
          }
        }
      } else {
        // Regular X position assignment for positions that still need resolution
        for (int pos = 0; pos < 4; pos++) {
          if (path[i].chip[pos] != -1 && path[i].x[pos] == -2) {
            int freeX = -1;
            
            // Find free X position using search order
            for (int searchIdx = 0; searchIdx < 16; searchIdx++) {
              if (freeXSearchOrder[path[i].chip[pos]][searchIdx] == -1) {
                continue;
              }
              
              int testX = freeXSearchOrder[path[i].chip[pos]][searchIdx];
              if (freeOrSameNetX(path[i].chip[pos], testX, path[i].net, allowStacking)) {
                freeX = testX;
                break;
              }
            }

            if (freeX != -1) {
              bool pathXSuccess = setPathX(i, pos, freeX);
              bool chipXSuccess = false;
              if (pathXSuccess) {
                chipXSuccess = setChipXStatus(path[i].chip[pos], freeX, path[i].net, "resolveUncommittedHops X");
              }

              if (!pathXSuccess || !chipXSuccess) {
                allAssignmentsSucceeded = false;
                break;
              }

              if (debugNTCC2) {
                Serial.print("  Assigned X=");
                Serial.print(freeX);
                Serial.print(" to position ");
                Serial.print(pos);
                Serial.print(" chip ");
                Serial.println(chipNumToChar(path[i].chip[pos]));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free X found for position ");
                Serial.print(pos);
                Serial.println();
              }
              break;
            }
          }
        }
      }
    }

    // Handle Y position assignments
    if (allAssignmentsSucceeded && hasUnresolvedY) {
      // For same-chip connections, find one Y position that works for both nodes
      if (path[i].sameChip && path[i].chip[0] == path[i].chip[1] && 
          path[i].y[0] == -2 && path[i].y[1] == -2) {
        
        int targetChip = path[i].chip[0];
        int sharedY = -1;
        
        // Find a free Y position that can be used for both nodes on the same chip
        for (int testY = 0; testY < 8; testY++) {
          // For breadboard chips, only allow Y=0
          if (targetChip < 8 && testY != 0) {
            continue;
          }
          
          if (freeOrSameNetY(targetChip, testY, path[i].net, allowStacking)) {
            // For special function chips, check breadboard chip X connection
            if (targetChip >= 8) {
              int bbChipX = xMapForChipLane0(testY, targetChip);
              if (bbChipX != -1) {
                if (!freeOrSameNetX(testY, bbChipX, path[i].net, allowStacking)) {
                  continue;
                }
              }
            }
            sharedY = testY;
            break;
          }
        }
        
        if (sharedY != -1) {
          // Assign the same Y position to both positions for same-chip connection
          bool success = setPathY(i, 0, sharedY) && setPathY(i, 1, sharedY);
          bool chipSuccess = false;
          if (success) {
            chipSuccess = setChipYStatusSafe(targetChip, sharedY, path[i].net, "resolveUncommittedHops same-chip Y");
          }
          
          // Update breadboard chip X status for special function chips
          if (success && chipSuccess && targetChip >= 8) {
            int bbChipX = xMapForChipLane0(sharedY, targetChip);
            if (bbChipX != -1) {
              bool xSuccess = setChipXStatus(sharedY, bbChipX, path[i].net, "resolveUncommittedHops same-chip BB X");
              if (!xSuccess) {
                allAssignmentsSucceeded = false;
              }
            }
          }
          
          if (!success || !chipSuccess) {
            allAssignmentsSucceeded = false;
          } else {
            if (debugNTCC2) {
              Serial.print("  Assigned shared Y=");
              Serial.print(sharedY);
              Serial.print(" to same-chip path[");
              Serial.print(i);
              Serial.print("] on chip ");
              Serial.println(chipNumToChar(targetChip));
            }
          }
        } else {
          allAssignmentsSucceeded = false;
          if (debugNTCC2) {
            Serial.print("ERROR: No free shared Y found for same-chip path[");
            Serial.print(i);
            Serial.println("]");
          }
        }
      } else {
        // Check if this is an ijkl inter-chip path
        bool isIjklPath = false;
      if (path[i].chip[0] != path[i].chip[1] && path[i].chip[2] != -1 && path[i].chip[3] != -1) {
        // Check if positions 0&2 are same chip and positions 1&3 are same chip
        if (path[i].chip[0] == path[i].chip[2] && path[i].chip[1] == path[i].chip[3]) {
          isIjklPath = true;
          if (debugNTCC2) {
            Serial.print("Detected ijkl inter-chip path[");
            Serial.print(i);
            Serial.print("] - positions 0&2 on chip ");
            Serial.print(chipNumToChar(path[i].chip[0]));
            Serial.print(", positions 1&3 on chip ");
            Serial.println(chipNumToChar(path[i].chip[1]));
          }
        }
      }

      if (isIjklPath) {
        // Handle ijkl paths: assign same Y to chip pairs (0&2, 1&3)
        int chipPairs[2][2] = {{0, 2}, {1, 3}}; // {pos0, pos2} and {pos1, pos3}
        
        for (int pairIdx = 0; pairIdx < 2; pairIdx++) {
          int pos1 = chipPairs[pairIdx][0];
          int pos2 = chipPairs[pairIdx][1];
          
          // Check if this chip pair needs Y resolution
          bool needsY = (path[i].chip[pos1] != -1 && path[i].y[pos1] == -2) ||
                        (path[i].chip[pos2] != -1 && path[i].y[pos2] == -2);
          
          if (needsY) {
            int targetChip = path[i].chip[pos1]; // Both positions should be same chip
            int freeY = -1;
            
            for (int testY = 0; testY < 8; testY++) {
              // For breadboard chips, only allow Y=0
              if (targetChip < 8 && testY != 0) {
                continue;
              }

              // Check if this Y position is free
              if (!freeOrSameNetY(targetChip, testY, path[i].net, allowStacking)) {
                continue;
              }

              // For special function chips, check breadboard chip X connection
              if (targetChip >= 8) {
                int bbChipX = xMapForChipLane0(testY, targetChip);
                if (bbChipX != -1) {
                  if (!freeOrSameNetX(testY, bbChipX, path[i].net, allowStacking)) {
                    continue;
                  }
                }
              }

              freeY = testY;
              break;
            }

            if (freeY != -1) {
              // Assign same Y to both positions in the pair
              bool success = true;
              
              if (path[i].y[pos1] == -2) {
                success &= setPathY(i, pos1, freeY);
              }
              if (path[i].y[pos2] == -2) {
                success &= setPathY(i, pos2, freeY);
              }
              
              bool chipYSuccess = setChipYStatusSafe(targetChip, freeY, path[i].net, "resolveUncommittedHops ijkl Y");
              
              if (!success || !chipYSuccess) {
                allAssignmentsSucceeded = false;
                if (debugNTCC2) {
                  Serial.print("ERROR: Failed ijkl Y assignment for chip ");
                  Serial.println(chipNumToChar(targetChip));
                }
                break;
              }

              // Update breadboard chip X status for special function chips
              if (targetChip >= 8) {
                int bbChipX = xMapForChipLane0(freeY, targetChip);
                if (bbChipX != -1) {
                  bool xSuccess = setChipXStatus(freeY, bbChipX, path[i].net, "resolveUncommittedHops ijkl BB X");
                  if (!xSuccess) {
                    allAssignmentsSucceeded = false;
                    if (debugNTCC2) {
                      Serial.print("ERROR: Failed ijkl BB X assignment for chip ");
                      Serial.println(chipNumToChar(freeY));
                    }
                    break;
                  }
                }
              }

              if (debugNTCC2) {
                Serial.print("  Assigned Y=");
                Serial.print(freeY);
                Serial.print(" to ijkl chip pair: positions ");
                Serial.print(pos1);
                Serial.print("&");
                Serial.print(pos2);
                Serial.print(" on chip ");
                Serial.println(chipNumToChar(targetChip));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free Y found for ijkl chip ");
                Serial.println(chipNumToChar(targetChip));
              }
              break;
            }
          }
        }
      } else {
        // Handle regular paths: assign Y to each position independently
        for (int pos = 0; pos < 4; pos++) {
          if (path[i].chip[pos] != -1 && path[i].y[pos] == -2) {
            int freeY = -1;
            
            for (int testY = 0; testY < 8; testY++) {
              // For breadboard chips, only allow Y=0
              if (path[i].chip[pos] < 8 && testY != 0) {
                continue;
              }

              // Check if this Y position is free
              if (!freeOrSameNetY(path[i].chip[pos], testY, path[i].net, allowStacking)) {
                continue;
              }

              // For special function chips, check breadboard chip X connection
              if (path[i].chip[pos] >= 8) {
                int bbChipX = xMapForChipLane0(testY, path[i].chip[pos]);
                if (bbChipX != -1) {
                  if (!freeOrSameNetX(testY, bbChipX, path[i].net, allowStacking)) {
                    continue;
                  }
                }
              }

              freeY = testY;
              break;
            }

            if (freeY != -1) {
              bool pathYSuccess = setPathY(i, pos, freeY);
              bool chipYSuccess = false;
              if (pathYSuccess) {
                chipYSuccess = setChipYStatusSafe(path[i].chip[pos], freeY, path[i].net, "resolveUncommittedHops Y");
              }

              if (!pathYSuccess || !chipYSuccess) {
                allAssignmentsSucceeded = false;
                break;
              }

              // Update breadboard chip X status for special function chips
              if (path[i].chip[pos] >= 8) {
                int bbChipX = xMapForChipLane0(freeY, path[i].chip[pos]);
                if (bbChipX != -1) {
                  bool xSuccess = setChipXStatus(freeY, bbChipX, path[i].net, "resolveUncommittedHops BB X for SF");
                  if (!xSuccess) {
                    allAssignmentsSucceeded = false;
                    break;
                  }
                }
              }

              if (debugNTCC2) {
                Serial.print("  Assigned Y=");
                Serial.print(freeY);
                Serial.print(" to position ");
                Serial.print(pos);
                Serial.print(" chip ");
                Serial.println(chipNumToChar(path[i].chip[pos]));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free Y found for position ");
                Serial.print(pos);
                Serial.println();
              }
              break;
            }
          }
        }
      }
      }
    }

    // Commit or restore based on success
    if (allAssignmentsSucceeded) {
      commitRoutingState();
      if (debugNTCC2) {
        Serial.print("SUCCESS: All positions resolved for path[");
        Serial.print(i);
        Serial.println("]");
      }
    } else {
      restoreRoutingState(i);
      path[i].altPathNeeded = true;
      if (debugNTCC2) {
        Serial.print("FAILED: Position assignments failed for path[");
        Serial.print(i);
        Serial.println("], state restored");
      }
    }
  }

  // Debug: Show final path states
  if (debugNTCC2) {
    Serial.println("Final path Y values after resolveUncommittedHops:");
    int unresolvedCount = 0;
    for (int i = 0; i < numberOfPaths; i++) {
      bool hasNegTwo = false;
      for (int j = 0; j < 4; j++) {
        if (path[i].y[j] == -2) {
          hasNegTwo = true;
          break;
        }
      }
      if (hasNegTwo) {
        unresolvedCount++;
        Serial.print("  path[");
        Serial.print(i);
        Serial.print("] net=");
        Serial.print(path[i].net);
        Serial.print(" (");
        printNodeOrName(path[i].node1);
        Serial.print("-");
        printNodeOrName(path[i].node2);
        Serial.print(") still has -2: ");
        Serial.print("chips=[");
        for (int j = 0; j < 4; j++) {
          if (path[i].chip[j] != -1) {
            Serial.print(chipNumToChar(path[i].chip[j]));
            if (j < 3 && path[i].chip[j + 1] != -1)
              Serial.print(",");
          }
        }
        Serial.print("] y=[");
        for (int j = 0; j < 4; j++) {
          Serial.print(path[i].y[j]);
          if (j < 3)
            Serial.print(",");
        }
        Serial.print("] altPathNeeded=");
        Serial.println(path[i].altPathNeeded);
      }
    }
    if (unresolvedCount == 0) {
      Serial.println("  All Y positions successfully resolved!");
    } else {
      Serial.print("  WARNING: ");
      Serial.print(unresolvedCount);
      Serial.println(" paths still have unresolved Y positions (-2)");
    }
  }
}



int checkForOverlappingPaths() {
  int found = 0;

  // printPathsCompact(2);
  // printChipStatus();

  for (int i = 0; i < numberOfPaths; i++) {
    int fchip[4] = {path[i].chip[0], path[i].chip[1], path[i].chip[2],
                    path[i].chip[3]};

    for (int j = 0; j < numberOfPaths; j++) {
      if (i == j) {
        continue;
      }
      if (path[i].net == path[j].net) {
        continue;
      }
      int schip[4] = {path[j].chip[0], path[j].chip[1], path[j].chip[2],
                      path[j].chip[3]};

      for (int f = 0; f < 4; f++) {
        for (int s = 0; s < 4; s++) {
          if (fchip[f] == schip[s] && fchip[f] != -1) {
            if (path[i].x[f] <= 0 || path[j].x[s] <= 0) {
              continue;
            }
            if (path[i].y[f] <= 0 || path[j].y[s] <= 0) {
              continue;
            }
            if (path[i].x[f] == path[j].x[s] && path[i].skip == 0 &&
                path[j].skip == 0) {
              // if (debugNTCC3) {
              if (path[i].duplicate > 0 || path[j].duplicate > 0) {
              if (path[i].duplicate > 0) {
                path[i].net = -1;
                path[i].duplicate = 0;
                path[i].chip[0] = -1;
                path[i].chip[1] = -1;
                path[i].chip[2] = -1;
                path[i].chip[3] = -1;
                path[i].x[0] = -1;
                path[i].x[1] = -1;
                path[i].x[2] = -1;
                path[i].x[3] = -1;
                path[i].y[0] = -1;
                path[i].y[1] = -1;
                path[i].y[2] = -1;
                path[i].y[3] = -1;
                path[i].altPathNeeded = false;
                path[i].sameChip = false;
                path[i].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at x ");
                // Serial.print(path[i].x[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(path[i].net);
                // Serial.print(" and ");
                // Serial.print(path[j].net);
                // Serial.println("   skipping");
                
              }
              if (path[j].duplicate > 0) {
                path[j].net = -1;
                path[j].duplicate = 0;
                path[j].chip[0] = -1;
                path[j].chip[1] = -1;
                path[j].chip[2] = -1;
                path[j].chip[3] = -1;
                path[j].x[0] = -1;
                path[j].x[1] = -1;
                path[j].x[2] = -1;
                path[j].x[3] = -1;
                path[j].y[0] = -1;
                path[j].y[1] = -1;
                path[j].y[2] = -1;
                path[j].y[3] = -1;
                path[j].altPathNeeded = false;
                path[j].sameChip = false;
                path[j].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at x ");
                // Serial.print(path[i].x[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(path[i].net);
                // Serial.print(" and ");
                // Serial.print(path[j].net);
                // Serial.println("   skipping");
               
              }
              continue;
              }

              Serial.print("Path ");
              Serial.print(i);
              Serial.print(" and ");
              Serial.print(j);
              Serial.print(" overlap at x ");
              Serial.print(path[i].x[f]);
              Serial.print(" on chip ");
              Serial.print(chipNumToChar(fchip[f]));
              Serial.print("   nets ");
              Serial.print(path[i].net);
              Serial.print(" and ");
              Serial.println(path[j].net);
              path[i].skip = 1;

              // printPathsCompact();
              // printChipStatus();
              // }
              // return 1;
              found++;
            } else if (path[i].y[f] == path[j].y[s] && path[i].skip == 0 &&
                       path[j].skip == 0) {
              // if (debugNTCC3) {
              if (path[i].duplicate > 0 || path[j].duplicate > 0) {
              if (path[i].duplicate > 0) {
                path[i].net = -1;
                path[i].duplicate = 0;
                path[i].chip[0] = -1;
                path[i].chip[1] = -1;
                path[i].chip[2] = -1;
                path[i].chip[3] = -1;
                path[i].x[0] = -1;
                path[i].x[1] = -1;
                path[i].x[2] = -1;
                path[i].x[3] = -1;
                path[i].y[0] = -1;
                path[i].y[1] = -1;
                path[i].y[2] = -1;
                path[i].y[3] = -1;
                path[i].altPathNeeded = false;
                path[i].sameChip = false;
                path[i].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at y ");
                // Serial.print(path[i].y[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(path[i].net);
                // Serial.print(" and ");
                // Serial.println(path[j].net);
                // Serial.println("   skipping");
              }
              if (path[j].duplicate > 0) {
                path[j].net = -1;
                path[j].duplicate = 0;
                path[j].chip[0] = -1;
                path[j].chip[1] = -1;
                path[j].chip[2] = -1;
                path[j].chip[3] = -1;
                path[j].x[0] = -1;
                path[j].x[1] = -1;
                path[j].x[2] = -1;
                path[j].x[3] = -1;
                path[j].y[0] = -1;
                path[j].y[1] = -1;
                path[j].y[2] = -1;
                path[j].y[3] = -1;
                path[j].altPathNeeded = false;
                path[j].sameChip = false;
                path[j].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at y ");
                // Serial.print(path[i].y[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(path[i].net);
                // Serial.print(" and ");
                // Serial.println(path[j].net);
                // Serial.println("   skipping");
              }
              continue;
              }

              Serial.print("Path ");
              Serial.print(i);
              Serial.print(" and ");
              Serial.print(j);
              Serial.print(" overlap at y ");
              Serial.print(path[i].y[f]);
              Serial.print(" on chip ");
              Serial.print(chipNumToChar(fchip[f]));
              Serial.print("   nets ");
              Serial.print(path[i].net);
              Serial.print(" and ");
              Serial.println(path[j].net);
              path[i].skip = 1;
              // printPathsCompact();
              // printChipStatus();
              // }
              // return 1;
              found++;
            }
          }
        }
      }
    }
  }
  return found;
}

int printNetOrNumber(int net) {
  int spaces = 0;
  switch (net) {
  case 0:
    spaces = Serial.print("E");
    break;
  case 1:
    spaces = Serial.print("Gn");
    break;
  case 2:
    spaces = Serial.print("T");
    break;
  case 3:
    spaces = Serial.print("B");
    break;
  case 4:
    spaces = Serial.print("d0");
    break;
  case 5:
    spaces = Serial.print("d1");
    break;
  default:
    spaces = Serial.print(net);
    break;
  }
  return spaces;
}

/// @brief print paths in a compact format
/// @param showCullDupes 0 = show all paths, 1 = show routed duplicates, 2 =
/// show all duplicates
void printPathsCompact(int showCullDupes) {
  // Serial.println(" ");
  // Serial.println(checkForOverlappingPaths());

  Serial.print("numberOfPaths: ");
  Serial.println(numberOfPaths);
  Serial.print("numberOfNets: ");
  Serial.println(numberOfNets);
  // Serial.println("showCullDupes: ");
  // Serial.println(showCullDupes);
  // Serial.println("numberOfBridges: ");
  // Serial.println(numberOfBridges);
  // Serial.println("numberOfNodes: ");
  assignTermColor();
  int lastDuplicate = 0;
  int duplicateSection = 0;

  int skipLine = 0;
  Serial.println(
      "\n\rpath\tnet\tnode1\tchip0\tx0\ty0\tnode2\tchip1\tx1\ty1\ta"
      "ltPath\tsameChp\tdup\tpathType\tchip2\tx2\ty2"); //\tx3\ty3\n\r");

  for (int i = 0; i < numberOfPaths; i++) {
    skipLine = 0;
    switch (showCullDupes) {
    case 0:
      if (path[i].duplicate > 0) {
        skipLine = 1;
        // continue;
      }
      break;
    case 1:

      if (path[i].duplicate > 0 && path[i].x[0] < 0 && path[i].x[1] < 0) {
        skipLine = 1;
        // continue;
      }
      break;
    }

    if (path[i].duplicate > 0 && duplicateSection == 0) {
      // Serial.println("\n\rduplicates");
      // duplicateSection = 1;
      skipLine = 1;
      // continue;
    }
    if (path[i].duplicate == 0 && duplicateSection == 1) {
      skipLine = 1;
      // continue;
    }

    if (skipLine == 0) {
      lastDuplicate = path[i].duplicate;
      changeTerminalColor(net[path[i].net].termColor);
      Serial.print(i);
      Serial.print("\t");

      // Serial.print(path[i].net);

      printNetOrNumber(path[i].net);
      Serial.print("\t");
      printNodeOrName(path[i].node1);
      // Serial.print("\t");
      // Serial.print(path[i].nodeType[0]);
      Serial.print("\t");
      Serial.print(chipNumToChar(path[i].chip[0]));
      Serial.print("\t");
      Serial.print(path[i].x[0]);
      Serial.print("\t");
      Serial.print(path[i].y[0]);
      Serial.print("\t");
      printNodeOrName(path[i].node2);
      // Serial.print("\t");
      // Serial.print(path[i].nodeType[1]);
      Serial.print("\t");
      Serial.print(chipNumToChar(path[i].chip[1]));
      Serial.print("\t");
      Serial.print(path[i].x[1]);
      Serial.print("\t");
      Serial.print(path[i].y[1]);
      Serial.print("\t");
      Serial.print(path[i].altPathNeeded);
      Serial.print("\t");
      Serial.print(path[i].sameChip);
      Serial.print("\t");
      Serial.print(path[i].duplicate);
      Serial.print("\t");
      printPathType(i);

      if (path[i].chip[2] != -1) {
        Serial.print(" \t");
        Serial.print(chipNumToChar(path[i].chip[2]));
        Serial.print(" \t");
        Serial.print(path[i].x[2]);
        Serial.print(" \t");
        Serial.print(path[i].y[2]);
        Serial.print(" \t");
        Serial.print(path[i].x[3]);
        Serial.print(" \t");
        Serial.print(path[i].y[3]);
      }
      if (1) {
        if (path[i].chip[3] != -1) {
          Serial.print(" \t");
          Serial.print(chipNumToChar(path[i].chip[3]));
          Serial.print(" \t");
        }
      }

      Serial.println(" ");
      Serial.flush();
    }

    if (showCullDupes > 0 && duplicateSection == 0 && i >= numberOfPaths - 1) {
      // if ( jumperlessConfig.routing.stack_paths > 0 ||
      // jumperlessConfig.routing.stack_rails > 0 ||
      // jumperlessConfig.routing.stack_dacs > 0) { Serial.print("numberOfPaths
      // = "); Serial.print(numberOfPaths); Serial.print("\ti = ");
      // Serial.print(i);

      duplicateSection = 1;
      changeTerminalColor();
      Serial.println("\n\rduplicates");
      i = 0;
    }
    changeTerminalColor();
  }

  // Serial.println(
  //     "\n\rpath\tnet\tnode1\tchip0\tx0\ty0\tnode2\tchip1\tx1\ty1\ta"
  //     "ltPath\tsameChp\tpathType\tchipL\tchip2\tx2\ty2\n\r");
}

void printChipStatus(void) {
  Serial.println(
      "\n\rchip\t0    1    2    3    4    5    6    7    8    9    10   "
      "11   "
      "12   13   14   15\t\t0    1    2    3    4    5    6    7");
  for (int i = 0; i < 12; i++) {
    int spaces = 0;
    Serial.print(chipNumToChar(i));
    Serial.print("\t");
    for (int j = 0; j < 16; j++) {
      if (ch[i].xStatus[j] == -1) {
        spaces += Serial.print(".");
      } else {
        changeTerminalColor(net[ch[i].xStatus[j]].termColor);
        spaces += printNetOrNumber(ch[i].xStatus[j]);
        changeTerminalColor();
      }
      for (int k = 0; k < 4 - spaces; k++) {
        Serial.print(" ");
      }
      Serial.print(" ");
      spaces = 0;
    }
    Serial.print("\t");
    for (int j = 0; j < 8; j++) {
      if (ch[i].yStatus[j] == -1) {
        spaces += Serial.print(".");
      } else {
        changeTerminalColor(net[ch[i].yStatus[j]].termColor);
        spaces += printNetOrNumber(ch[i].yStatus[j]);
        changeTerminalColor();
      }

      for (int k = 0; k < 4 - spaces; k++) {
        Serial.print(" ");
      }
      Serial.print(" ");
      spaces = 0;
    }
    if (i == 7) {
      Serial.print("\n\n\rchip\t0    1    2    3    4    5    6    7    "
                   "8    9    10   "
                   "11   12   13   14   15\t\t0    1    2    3    4    5 "
                   "   6    7");
    }
    Serial.println(" ");
  }
}

void findStartAndEndChips(int node1, int node2, int pathIdx) {
  if (debugNTCC2) {
    Serial.print("findStartAndEndChips()\n\r");
  }
  bothNodes[0] = node1;
  bothNodes[1] = node2;
  startEndChip[0] = -1;
  startEndChip[1] = -1;

  if (debugNTCC5) {
    Serial.print("finding chips for nodes: ");
    Serial.print(definesToChar(node1));
    Serial.print("-");
    Serial.println(definesToChar(node2));
  }

  for (int twice = 0; twice < 2; twice++) // first run gets node1 and start
                                          // chip, second is node2 and end
  {
    if (debugNTCC5) {
      Serial.print("node: ");
      Serial.println(twice + 1);
      Serial.println(" ");
    }
    int candidatesFound = 0;

    switch (bothNodes[twice]) {
    case 30:
    case 60: {
      path[pathIdx].chip[twice] = CHIP_L;
      if (debugNTCC5) {
        Serial.print("chip: ");
        Serial.println(chipNumToChar(path[pathIdx].chip[twice]));
      }
      break;
    }

    case 29:
    case 59: {
      path[pathIdx].chip[twice] = CHIP_K;
      if (debugNTCC5) {
        Serial.print("chip: ");
        Serial.println(chipNumToChar(path[pathIdx].chip[twice]));
      }
      break;
    }

    case 1 ... 28: // on the breadboard
    case 31 ... 58: {
      path[pathIdx].chip[twice] = bbNodesToChip[bothNodes[twice]];
      if (debugNTCC5) {
        Serial.print("chip: ");
        Serial.println(chipNumToChar(path[pathIdx].chip[twice]));
      }
      break;
    }
    case NANO_D0 ... NANO_A7: // on the nano
    {
      int nanoIndex = defToNano(bothNodes[twice]);

      if (nano.numConns[nanoIndex] == 1) {
        path[pathIdx].chip[twice] = nano.mapIJ[nanoIndex];
        if (debugNTCC5) {
          Serial.print("nano chip: ");
          Serial.println(chipNumToChar(path[pathIdx].chip[twice]));
        }
      } else {
        if (debugNTCC5) {
          Serial.print("nano candidate chips: ");
        }
        chipCandidates[twice][0] = nano.mapIJ[nanoIndex];
        path[pathIdx].candidates[twice][0] = chipCandidates[twice][0];
        // Serial.print(candidatesFound);
        if (debugNTCC5) {
          Serial.print(chipNumToChar(path[pathIdx].candidates[twice][0]));
        }
        candidatesFound++;
        chipCandidates[twice][1] = nano.mapKL[nanoIndex];
        Serial.print(candidatesFound);
        path[pathIdx].candidates[twice][1] = chipCandidates[twice][1];
        candidatesFound++;
        if (debugNTCC5) {
          Serial.print(" ");
          Serial.println(chipNumToChar(path[pathIdx].candidates[twice][1]));
        }
      }
      break;
    }
    case GND ... 141: {
      if (debugNTCC5) {
        Serial.print("special function candidate chips: ");
      }
      for (int i = 8; i < 12; i++) {
        for (int j = 0; j < 16; j++) {
          if (ch[i].xMap[j] == bothNodes[twice]) {
            chipCandidates[twice][candidatesFound] = i;
            path[pathIdx].candidates[twice][candidatesFound] =
                chipCandidates[twice][candidatesFound];
            candidatesFound++;
            if (debugNTCC5) {
              Serial.print(chipNumToChar(i));
              Serial.print(" ");
            }
          }
        }
      }

      if (candidatesFound == 1) {
        path[pathIdx].chip[twice] = chipCandidates[twice][0];

        path[pathIdx].candidates[twice][0] = -1;
        if (debugNTCC5) {
          Serial.print("chip: ");
          Serial.println(chipNumToChar(path[pathIdx].chip[twice]));
        }
      }
      if (debugNTCC5) {
        Serial.println(" ");
      }
      break;
    }
    }
  }
}

void mergeOverlappingCandidates(
    int pathIndex) // also sets altPathNeeded flag if theyre on different
// sf chips (there are no direct connections between
// them)
{
  // Serial.print("\t 0 \t");
  int foundOverlap = 0;

  if ((path[pathIndex].candidates[0][0] != -1 &&
       path[pathIndex].candidates[1][0] != -1)) // if both nodes have candidates
  {
    /// Serial.print("\t1");
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 3; j++) {
        if (path[pathIndex].candidates[0][i] ==
            path[pathIndex].candidates[1][j]) {
          // Serial.print("! \t");
          path[pathIndex].chip[0] = path[pathIndex].candidates[0][i];
          path[pathIndex].chip[1] = path[pathIndex].candidates[0][i];
          foundOverlap = 1;
          break;
        }
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }
  } else if (path[pathIndex].candidates[0][0] != -1) // if node 1 has candidates
  {
    // Serial.print("\t2");

    for (int j = 0; j < 3; j++) {
      if (path[pathIndex].chip[1] == path[pathIndex].candidates[0][j]) {
        // Serial.print("! \t");
        path[pathIndex].chip[0] = path[pathIndex].candidates[0][j];

        foundOverlap = 1;

        break;
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }

    // path[pathIndex].altPathNeeded = 1;
  } else if (path[pathIndex].candidates[1][0] != -1) // if node 2 has candidates
  {
    // Serial.print(" \t3");

    for (int j = 0; j < 3; j++) {
      if (path[pathIndex].chip[0] == path[pathIndex].candidates[1][j]) {
        // Serial.print("! \t");

        path[pathIndex].chip[1] = path[pathIndex].candidates[1][j];
        foundOverlap = 1;
        break;
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }

    // path[pathIndex].altPathNeeded = 1;
  }

  if (foundOverlap == 1) {
    path[pathIndex].candidates[0][0] = -1;
    path[pathIndex].candidates[0][1] = -1;
    path[pathIndex].candidates[0][2] = -1;
    path[pathIndex].candidates[1][0] = -1;
    path[pathIndex].candidates[1][1] = -1;
    path[pathIndex].candidates[1][2] = -1;
  } else {
  }

  //   if (path[pathIndex].chip[0] >= CHIP_I && path[pathIndex].chip[1] >=
  //   CHIP_I) {
  //     if (path[pathIndex].chip[0] != path[pathIndex].chip[1]) {

  //       path[pathIndex].altPathNeeded = 1;
  //     }
  //   }
}

void assignPathType(int pathIndex) {
  if (path[pathIndex].chip[0] == path[pathIndex].chip[1]) {
    path[pathIndex].sameChip = true;
  } else {
    path[pathIndex].sameChip = false;
  }

  if ((path[pathIndex].node1 == 29 || path[pathIndex].node1 == 59 ||
       path[pathIndex].node1 == 30 || path[pathIndex].node1 == 60) ||
      path[pathIndex].node1 == 114 || path[pathIndex].node1 == 116 ||
      path[pathIndex].node1 == 117) {
    // Serial.print("\n\n\rthis should be a bb to sf connection\n\n\n\r
    // ");
    //  path[pathIndex].altPathNeeded = true;
    // Serial.print("path ");
    // Serial.print(pathIndex);
    // Serial.print(" is a bb to sf connection, swapping\n\r");
    // Serial.print("node1: ");
    // Serial.print(path[pathIndex].node1);
    // Serial.print("\tnode2: ");
    // Serial.print(path[pathIndex].node2);
    // Serial.println("\n\r");
    swapNodes(pathIndex);
    //     Serial.print("path ");
    // Serial.print(pathIndex);
    // Serial.print(" is a bb to sf connection, swapping\n\r");
    // Serial.print("node1: ");
    // Serial.print(path[pathIndex].node1);
    // Serial.print("\tnode2: ");
    // Serial.print(path[pathIndex].node2);
    // Serial.println("\n\r");
    // path[pathIndex].Lchip = true;

    path[pathIndex].nodeType[0] = SF; // maybe have a separate type for ChipL
    // connected nodes, but not now
  }

  if ((path[pathIndex].node1 >= 1 && path[pathIndex].node1 <= 28) ||
      (path[pathIndex].node1 >= 31 && path[pathIndex].node1 <= 58)) {
    path[pathIndex].nodeType[0] = BB;
  } else if (path[pathIndex].node1 >= NANO_D0 &&
             path[pathIndex].node1 <= NANO_A7) {
    path[pathIndex].nodeType[0] = NANO;
  } else if (path[pathIndex].node1 >= GND && path[pathIndex].node1 <= 141) {
    path[pathIndex].nodeType[0] = SF;
  }

  if ((path[pathIndex].node2 == 29 || path[pathIndex].node2 == 59 ||
       path[pathIndex].node2 == 30 || path[pathIndex].node2 == 60) ||
      path[pathIndex].node2 == 114 || path[pathIndex].node2 == 116 ||
      path[pathIndex].node2 == 117 || path[pathIndex].chip[1] == CHIP_K) {
    // Serial.print("\n\n\rthis should be a bb to sf connection 2\n\n\n\r
    // "); path[pathIndex].altPathNeeded = true; path[pathIndex].Lchip =
    // true;
    path[pathIndex].nodeType[1] = SF;
  } else if ((path[pathIndex].node2 >= 1 && path[pathIndex].node2 <= 28) ||
             (path[pathIndex].node2 >= 31 && path[pathIndex].node2 <= 58)) {
    path[pathIndex].nodeType[1] = BB;
  } else if (path[pathIndex].node2 >= NANO_D0 &&
             path[pathIndex].node2 <= NANO_A7) {
    path[pathIndex].nodeType[1] = NANO;
  } else if (path[pathIndex].node2 >= GND && path[pathIndex].node2 <= 141) {
    path[pathIndex].nodeType[1] = SF;
  }

  if ((path[pathIndex].nodeType[0] == NANO &&
       path[pathIndex].nodeType[1] == SF)) {
    path[pathIndex].pathType = NANOtoSF;
    if (path[pathIndex].chip[0] != path[pathIndex].chip[1]) {
      path[pathIndex].altPathNeeded = true;
    }
  } else if ((path[pathIndex].nodeType[0] == SF &&
              path[pathIndex].nodeType[1] == SF)) {
    path[pathIndex].pathType =
        NANOtoSF; // SFtoSF is dealt with the same as NANOtoSF
    // Serial.print("pathIndex: ");
    // Serial.println(pathIndex);
    // Serial.print("path[pathIndex].pathType: ");
    // Serial.println(path[pathIndex].pathType);
    path[pathIndex].altPathNeeded = true;
  } else if ((path[pathIndex].nodeType[0] == SF &&
              path[pathIndex].nodeType[1] == NANO)) {
    // swapNodes(pathIndex);
    path[pathIndex].pathType = NANOtoSF;
    if (path[pathIndex].chip[0] != path[pathIndex].chip[1]) {
      path[pathIndex].altPathNeeded = true;
    }

    // path[pathIndex].altPathNeeded = true;
  } else if ((path[pathIndex].nodeType[0] == BB &&
              path[pathIndex].nodeType[1] == SF)) {
    path[pathIndex].pathType = BBtoSF;
  } else if ((path[pathIndex].nodeType[0] == SF &&
              path[pathIndex].nodeType[1] == BB)) {
    swapNodes(pathIndex);
    path[pathIndex].pathType = BBtoSF;
  } else if ((path[pathIndex].nodeType[0] == BB &&
              path[pathIndex].nodeType[1] == NANO)) {
    path[pathIndex].pathType = BBtoNANO;
  } else if (path[pathIndex].nodeType[0] == NANO &&
             path[pathIndex].nodeType[1] ==
                 BB) // swtich node order so BB always comes first
  {
    swapNodes(pathIndex);
    path[pathIndex].pathType = BBtoNANO;
  } else if (path[pathIndex].nodeType[0] == BB &&
             path[pathIndex].nodeType[1] == BB) {
    path[pathIndex].pathType = BBtoBB;
  } else if (path[pathIndex].nodeType[0] == NANO &&
             path[pathIndex].nodeType[1] == NANO) {
    path[pathIndex].pathType = NANOtoNANO;
  }
  if (debugNTCC) {
    Serial.print("Path ");
    Serial.print(pathIndex);
    Serial.print(" type: ");
    printPathType(pathIndex);
    Serial.print("\n\r");

    Serial.print("  Node 1: ");
    Serial.print(path[pathIndex].node1);
    Serial.print("\tNode 2: ");
    Serial.print(path[pathIndex].node2);
    Serial.print("\n\r");

    Serial.print("  Chip 1: ");
    Serial.print(path[pathIndex].chip[0]);
    Serial.print("\tChip 2: ");
    Serial.print(path[pathIndex].chip[1]);
    Serial.print("\n\r");
  }
}

void swapNodes(int pathIndex) {
  int temp = 0;
  temp = path[pathIndex].node1;
  path[pathIndex].node1 = path[pathIndex].node2;
  path[pathIndex].node2 = temp;

  temp = path[pathIndex].chip[0];
  path[pathIndex].chip[0] = path[pathIndex].chip[1];
  path[pathIndex].chip[1] = temp;

  temp = path[pathIndex].candidates[0][0];
  path[pathIndex].candidates[0][0] = path[pathIndex].candidates[1][0];
  path[pathIndex].candidates[1][0] = temp;

  temp = path[pathIndex].candidates[0][1];
  path[pathIndex].candidates[0][1] = path[pathIndex].candidates[1][1];
  path[pathIndex].candidates[1][1] = temp;

  temp = path[pathIndex].candidates[0][2];
  path[pathIndex].candidates[0][2] = path[pathIndex].candidates[1][2];
  path[pathIndex].candidates[1][2] = temp;

  enum nodeType tempNT = path[pathIndex].nodeType[0];
  path[pathIndex].nodeType[0] = path[pathIndex].nodeType[1];
  path[pathIndex].nodeType[1] = tempNT;

  temp = path[pathIndex].x[0];
  path[pathIndex].x[0] = path[pathIndex].x[1];
  path[pathIndex].x[1] = temp;

  temp = path[pathIndex].y[0];
  path[pathIndex].y[0] = path[pathIndex].y[1];
  path[pathIndex].y[1] = temp;
}

int xMapForNode(int node, int chip) {
  int nodeFound = -1;
  for (int i = 0; i < 16; i++) {
    if (ch[chip].xMap[i] == node) {
      nodeFound = i;
      break;
    }
  }
  if (nodeFound == -1) {
    if (debugNTCC) {
      Serial.print("xMapForNode: \n\rnode ");
      Serial.print(node);
      Serial.print(" not found on chip ");
      Serial.println(chipNumToChar(chip));
    }
  }

  return nodeFound;
}

int yMapForNode(int node, int chip) {
  int nodeFound = -1;
  for (int i = 1; i < 8; i++) {
    if (ch[chip].yMap[i] == node) {
      nodeFound = i;
      break;
    }
  }
  return nodeFound;
}

int xMapForChipLane0(int chip1, int chip2) {
  int nodeFound = -1;
  for (int i = 0; i < 16; i++) {
    if (ch[chip1].xMap[i] == chip2) {
      nodeFound = i;
      break;
    }
  }
  return nodeFound;
}
int xMapForChipLane1(int chip1, int chip2) {
  int nodeFound = -1;
  for (int i = 0; i < 16; i++) {
    if (ch[chip1].xMap[i] == chip2) {
      if (ch[chip1].xMap[i + 1] == chip2) {
        nodeFound = i + 1;
        break;
      }
    }
  }

  if (nodeFound == -1) {
    if (debugNTCC) {
      Serial.print("nodeNotFound lane 1: ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(" ");
      Serial.println(chipNumToChar(chip2));
    }
  }

  return nodeFound;
}

void resolveChipCandidates(void) {
  int nodesToResolve[2] = {
      0, 0}; // {node1,node2} 0 = already found, 1 = needs resolving

  static int gndChipAlternator = 0; // Static counter for alternating GND chips

  for (int pathIndex = 0; pathIndex < numberOfPaths; pathIndex++) {
    // For duplicate path handling with stack_rails > 0, only process GND paths
    // Return early if not GND net since it's the only one with multiple routes
    if (path[pathIndex].duplicate == 1 && jumperlessConfig.routing.stack_rails > 0) {
      if (path[pathIndex].net != 1) {
        continue; // Skip non-GND nets for duplicate path processing when stacking
      }
      
      if (debugNTCC) {
        Serial.print("Processing GND duplicate path[");
        Serial.print(pathIndex);
        Serial.println("] with stacking enabled");
      }
    }

    nodesToResolve[0] = 0;
    nodesToResolve[1] = 0;

    if (path[pathIndex].chip[0] == -1) {
      nodesToResolve[0] = 1;
    } else {
      nodesToResolve[0] = 0;
    }

    if (path[pathIndex].chip[1] == -1) {
      nodesToResolve[1] = 1;
    } else {
      nodesToResolve[1] = 0;
    }

    for (int nodeOneOrTwo = 0; nodeOneOrTwo < 2; nodeOneOrTwo++) {
      if (nodesToResolve[nodeOneOrTwo] == 1) {
        // Check if this is a GND path (net 1)
        bool isGndPath = (path[pathIndex].net == 1);
        bool isGndDuplicateWithStacking = (isGndPath && path[pathIndex].duplicate == 1 && jumperlessConfig.routing.stack_rails > 0);
        int selectedChip = -1;

        if (isGndPath) {
          // Special handling for GND paths to balance between chips K and L
          int chipK = -1, chipL = -1;
          
          // Find K and L in the candidates
          for (int candIdx = 0; candIdx < 3; candIdx++) {
            if (path[pathIndex].candidates[nodeOneOrTwo][candIdx] == CHIP_K) {
              chipK = CHIP_K;
            } else if (path[pathIndex].candidates[nodeOneOrTwo][candIdx] == CHIP_L) {
              chipL = CHIP_L;
            }
          }

          if (chipK != -1 && chipL != -1) {
            // Both K and L are candidates
            if (isGndDuplicateWithStacking) {
              // For GND duplicate paths with stacking enabled, ensure we use both chips
              // Use the opposite chip from what was chosen for the primary path
              bool primaryUsesK = false, primaryUsesL = false;
              
              // Check what chips are already used by non-duplicate GND paths
              for (int checkPath = 0; checkPath < pathIndex; checkPath++) {
                if (path[checkPath].net == 1 && path[checkPath].duplicate == 0) {
                  if (path[checkPath].chip[0] == CHIP_K || path[checkPath].chip[1] == CHIP_K) {
                    primaryUsesK = true;
                  }
                  if (path[checkPath].chip[0] == CHIP_L || path[checkPath].chip[1] == CHIP_L) {
                    primaryUsesL = true;
                  }
                }
              }
              
              // For duplicates, prefer the chip that's less used, or alternate if both are used
              if (primaryUsesK && !primaryUsesL) {
                selectedChip = chipL;
              } else if (primaryUsesL && !primaryUsesK) {
                selectedChip = chipK;
              } else {
                // Both or neither used, alternate
                selectedChip = (gndChipAlternator % 2 == 0) ? chipK : chipL;
                gndChipAlternator++;
              }
              
              if (debugNTCC) {
                Serial.print("GND duplicate path[");
                Serial.print(pathIndex);
                Serial.print("] stacking enabled, selected chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.print(" (primaryUsesK=");
                Serial.print(primaryUsesK);
                Serial.print(", primaryUsesL=");
                Serial.print(primaryUsesL);
                Serial.println(")");
              }
            } else if (jumperlessConfig.routing.stack_rails > 0) {
              // When stacking is enabled, prefer the less crowded chip but allow both
              selectedChip = moreAvailableChip(chipK, chipL);
              
              if (debugNTCC) {
                Serial.print("GND path[");
                Serial.print(pathIndex);
                Serial.print("] stacking enabled, selected chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.println(" (less crowded)");
              }
            } else {
              // Alternate between K and L when not stacking
              selectedChip = (gndChipAlternator % 2 == 0) ? chipK : chipL;
              gndChipAlternator++;
              
              if (debugNTCC) {
                Serial.print("GND path[");
                Serial.print(pathIndex);
                Serial.print("] alternating to chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.print(" (alternator: ");
                Serial.print(gndChipAlternator - 1);
                Serial.println(")");
              }
            }
          } else if (chipK != -1) {
            selectedChip = chipK;
          } else if (chipL != -1) {
            selectedChip = chipL;
          } else {
            // Fall back to standard selection if neither K nor L found
            selectedChip = moreAvailableChip(path[pathIndex].candidates[nodeOneOrTwo][0],
                                           path[pathIndex].candidates[nodeOneOrTwo][1]);
          }
        } else {
          // Standard chip selection for non-GND paths
          selectedChip = moreAvailableChip(path[pathIndex].candidates[nodeOneOrTwo][0],
                                         path[pathIndex].candidates[nodeOneOrTwo][1]);
        }

        path[pathIndex].chip[nodeOneOrTwo] = selectedChip;
        
        if (debugNTCC && !isGndPath) {
          Serial.print("path[");
          Serial.print(pathIndex);
          Serial.print("] chip from ");
          Serial.print(
              chipNumToChar(path[pathIndex].chip[(1 + nodeOneOrTwo) % 2]));
          Serial.print(" to chip ");
          Serial.print(chipNumToChar(path[pathIndex].chip[nodeOneOrTwo]));
          Serial.print(" chosen\n\n\r");
        }
      }
    }
  }
}

int moreAvailableChip(int chip1, int chip2) {
  int chipChosen = -1;
  sortSFchipsLeastToMostCrowded();
  sortAllChipsLeastToMostCrowded();

  for (int i = 0; i < 12; i++) {
    if (chipsLeastToMostCrowded[i] == chip1 ||
        chipsLeastToMostCrowded[i] == chip2) {
      chipChosen = chipsLeastToMostCrowded[i];
      break;
    }
  }
  return chipChosen;
}

void sortSFchipsLeastToMostCrowded(void) {
  bool tempDebug = debugNTCC;
  // debugNTCC = false;
  int numberOfConnectionsPerSFchip[4] = {0, 0, 0, 0};

  for (int i = 0; i < numberOfPaths; i++) {
    for (int j = 0; j < 2; j++) {
      if (path[i].chip[j] > 7) {
        numberOfConnectionsPerSFchip[path[i].chip[j] - 8]++;
      }
    }
  }

  if (debugNTCC) {
    for (int i = 0; i < 4; i++) {
      Serial.print("sf connections: ");
      Serial.print(chipNumToChar(i + 8));
      Serial.print(numberOfConnectionsPerSFchip[i]);
      Serial.print("\n\r");
    }
  }
  // debugNTCC = tempDebug;
}

void sortAllChipsLeastToMostCrowded(void) {
  // bool tempDebug = debugNTCC;
  // debugNTCC = false;

  int numberOfConnectionsPerChip[12] = {
      0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0}; // this will be used to determine which chip is most
  // crowded

  for (int i = 0; i < 12; i++) {
    chipsLeastToMostCrowded[i] = i;
  }

  if (debugNTCC) {
    // Serial.println("\n\r");
  }
  for (int i = 0; i < numberOfPaths; i++) {
    for (int j = 0; j < 2; j++) {
      if (path[i].chip[j] != -1) {
        numberOfConnectionsPerChip[path[i].chip[j]]++;
      }
    }
  }

  // debugNTCC = false;
  if (debugNTCC5) {
    for (int i = 0; i < 12; i++) {
      Serial.print(chipNumToChar(i));
      Serial.print(": ");
      Serial.println(numberOfConnectionsPerChip[i]);
    }

    Serial.println("\n\r");
  }

  int temp = 0;

  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 11; j++) {
      if (numberOfConnectionsPerChip[j] > numberOfConnectionsPerChip[j + 1]) {
        temp = numberOfConnectionsPerChip[j];
        // chipsLeastToMostCrowded[j] = chipsLeastToMostCrowded[j + 1];
        numberOfConnectionsPerChip[j] = numberOfConnectionsPerChip[j + 1];
        numberOfConnectionsPerChip[j + 1] = temp;

        temp = chipsLeastToMostCrowded[j];
        chipsLeastToMostCrowded[j] = chipsLeastToMostCrowded[j + 1];
        chipsLeastToMostCrowded[j + 1] = temp;
      }
    }
  }

  for (int i = 0; i < 12; i++) {
    if (debugNTCC5) {
      Serial.print(chipNumToChar(chipsLeastToMostCrowded[i]));
      Serial.print(": ");
      Serial.println(numberOfConnectionsPerChip[i]);
    }
  }

  /*
      if (debugNTCC == true)
      {
          for (int i = 0; i < 4; i++)
          {
              Serial.print("\n\r");
              Serial.print(chipNumToChar(sfChipsLeastToMostCrowded[i]));
              Serial.print(": ");

              Serial.print("\n\r");
          }
      }
  */
  // debugNTCC = tempDebug;
  //  bbToSfConnections();
}

void printPathArray(void) // this also prints candidates and x y
{
  // Serial.print("\n\n\r");
  // Serial.print("newBridgeIndex = ");
  // Serial.println(newBridgeIndex);
  Serial.print("\n\r");
  int tabs = 0;
  int lineCount = 0;
  for (int i = 0; i < numberOfPaths; i++) {
    Serial.print("\n\r");
    tabs += Serial.print(i);
    Serial.print("  ");
    if (i < 10) {
      tabs += Serial.print(" ");
    }
    if (i < 100) {
      tabs += Serial.print(" ");
    }
    tabs += Serial.print("[");
    tabs += printNodeOrName(path[i].node1);
    tabs += Serial.print("-");
    tabs += printNodeOrName(path[i].node2);
    tabs += Serial.print("]\tNet ");
    tabs += printNodeOrName(path[i].net);
    tabs += Serial.println(" ");
    tabs += Serial.print("\n\rnode1 chip:  ");
    tabs += printChipNumToChar(path[i].chip[0]);
    tabs += Serial.print("\n\rnode2 chip:  ");
    tabs += printChipNumToChar(path[i].chip[1]);
    // tabs += Serial.print("\n\n\rnode1 candidates: ");
    // for (int j = 0; j < 3; j++) {
    //   printChipNumToChar(path[i].candidates[0][j]);
    //   tabs += Serial.print(" ");
    // }
    // tabs += Serial.print("\n\rnode2 candidates: ");
    // for (int j = 0; j < 3; j++) {
    //   printChipNumToChar(path[i].candidates[1][j]);
    //   tabs += Serial.print(" ");
    // }
    tabs += Serial.print("\n\rpath type: ");
    tabs += printPathType(i);

    if (path[i].altPathNeeded == true) {
      tabs += Serial.print("\n\ralt path needed");
    } else {
    }
    tabs += Serial.println("\n\n\r");

    /// Serial.print(tabs);
    for (int i = 0; i < 24 - (tabs); i++) {
      Serial.print(" ");
    }
    tabs = 0;
  }
}

int printPathType(int pathIndex) {
  switch (path[pathIndex].pathType) {
  case 0:
    return Serial.print("BB to BB");
    break;
  case 1:
    return Serial.print("BB to NANO");
    break;
  case 2:
    return Serial.print("NANO to NANO");
    break;
  case 3:
    return Serial.print("BB to SF");
    break;
  case 4:
    return Serial.print("NANO to SF");
    break;
  default:
    return Serial.print("Not Assigned");
    break;
  }
}

int defToNano(int nanoIndex) { return nanoIndex - NANO_D0; }

char chipNumToChar(int chipNumber) { return chipNumber + 'A'; }

int printChipNumToChar(int chipNumber) {
  return Serial.print(chipNumber);
  if (chipNumber == -1) {
    return Serial.print("-1");
  } else {
    return Serial.print((char)(chipNumber + 'A'));
  }
}

void clearChipsOnPathToNegOne(void) {
  for (int i = 0; i < MAX_BRIDGES - 1; i++) {
    if (i >= numberOfPaths) {
      path[i].node1 = 0; // i know i can just do {-1,-1,-1} but
      path[i].node2 = 0;
      path[i].net = 0;
      // Serial.println(i);
    }
    for (int c = 0; c < 4; c++) {
      path[i].chip[c] = -1;
    }

    for (int c = 0; c < 6; c++) {
      path[i].x[c] = -1;
      path[i].y[c] = -1;
    }

    for (int c = 0; c < 3; c++) {
      path[i].candidates[c][0] = -1;
      path[i].candidates[c][1] = -1;
      path[i].candidates[c][2] =
          -1; // CEEEEEEEE!!!!!! i had this set to 3 and it was clearing
      // everything, but no im not using rust
    }
  }
}




/*
So the nets are all made, now we need to figure out which chip connections
need to be made to make that phycially happen

start with the special function nets, they're the highest priority

maybe its simpler to make an array of every possible connection


start at net 1 and go up

find start and end chip

bb chips
sf chips
nano chips


things that store x and y valuse for paths
chipStatus.xStatus[]
chipStatus.yStatus[]
nanoStatus.xStatusIJKL[]


struct nanoStatus {  //there's only one of these so ill declare and initalize
together unlike above

//all these arrays should line up (both by index and visually) so one index
will give you all this data

//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const char *pinNames[24]=  {
" D0",   " D1",   " D2",   " D3",   " D4",   " D5",   " D6",   " D7",   " D8",
" D9",    "D10",    "D11",     "D12",    "D13",      "RST",     "REF",   "
A0", " A1",   " A2",   " A3",   " A4",   " A5",   " A6",   " A7"};// String
with readable name //padded to 3 chars (space comes before chars)
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t pinMap[24] =  {
NANO_D0, NANO_D1, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7,
NANO_D8, NANO_D9, NANO_D10, NANO_D11,  NANO_D12, NANO_D13, NANO_RESET,
NANO_AREF, NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6,
NANO_A7};//Array index to internal arbitrary #defined number
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t numConns[24]= {
1 , 1      , 2      , 2      , 2      , 2      , 2      , 2      , 2      , 2
, 2 , 2       ,  2       , 2       , 1         , 1        , 2      , 2      ,
2 , 2 , 2      , 2      , 1      , 1      };//Whether this pin has 1 or 2
connections to special function chips    (OR maybe have it be a map like i = 2
j = 3  k = 4 l = 5 if there's 2 it's the product of them ij = 6  ik = 8  il =
10 jk = 12 jl = 15 kl = 20 we're trading minuscule amounts of CPU for RAM)
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t  mapIJ[24] =  {
CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J
, CHIP_I , CHIP_J  , CHIP_I  ,  CHIP_J  , CHIP_I  , CHIP_I    ,  CHIP_J  ,
CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J
};//Since there's no overlapping connections between Chip I and J, this holds
which of those 2 chips has a connection at that index, if numConns is 1, you
only need to check this one const int8_t  mapKL[24] =  { -1     , -1     ,
CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K
, CHIP_K  ,  CHIP_K  , -1 , -1        , -1       , CHIP_K , CHIP_K , CHIP_K ,
CHIP_K , CHIP_L , CHIP_L , -1     , -1     };//Since there's no overlapping
connections between Chip K and L, this holds which of those 2 chips has a
connection at that index, -1 for no connection
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapI[24]  =  {
-1 , 1      , -1     , 3      , -1     , 5      , -1     , 7      , -1     , 9
, -1 , 8       ,  -1      , 10      , 11        , -1       , 0      , -1     ,
2 , -1 , 4      , -1     , 6      , -1     };//holds which X pin is connected
to the index on Chip I, -1 for none int8_t xStatusI[24]  =  { -1     , 0 , -1
, 0 , -1     , 0      , -1     , 0      , -1     , 0      , -1      , 0 ,  -1
, 0       , 0         , -1       , 0      , -1     , 0      , -1     , 0 , -1
, 0      , -1     };//-1 for not connected to that chip, 0 for available, >0
means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapJ[24]  =  {
0 , -1     , 2      , -1     , 4      , -1     , 6      , -1     , 8      , -1
, 9       , -1      ,  10      , -1      , -1        , 11       , -1     , 1 ,
-1 , 3      , -1     , 5      , -1     , 7      };//holds which X pin is
connected to the index on Chip J, -1 for none int8_t xStatusJ[24]  =  { 0 , -1
, 0      , -1     , 0      , -1     , 0      , -1     , 0      , -1     , 0 ,
-1 , 0        , 0       , -1        , 0        , -1     , 0      , -1     , 0
, -1 , 0      , -1     , 0      };//-1 for not connected to that chip, 0 for
available, >0 means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapK[24]  =  {
-1 , -1     , 4      , 5      , 6      , 7      , 8      , 9      , 10     ,
11 , 12      , 13      ,  14      , -1      , -1        , -1       , 0      ,
1 , 2 , 3      , -1     , -1     , -1     , -1     };//holds which X pin is
connected to the index on Chip K, -1 for none int8_t xStatusK[24]  =  { -1 ,
-1     , 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0 , 0
, 0 , 0       , -1      , -1        , -1       , 0      , 0      , 0      , 0
, -1     , -1     , -1     , -1     };//-1 for not connected to that chip, 0
for available, >0 means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapL[24]  =  {
-1 , -1     , -1     , -1     , -1     , -1     , -1     , -1     , -1     ,
-1 , -1      , -1      ,  -1      , -1      , -1        , -1       , -1     ,
-1 , -1 , -1     , 12     , 13     , -1     , -1     };//holds which X pin is
connected to the index on Chip L, -1 for none int8_t xStatusL[24]  =  { -1 ,
-1     , -1     , -1     , -1     , -1     , -1     , -1     , -1     , -1 ,
-1 , -1 ,  -1      , -1      , -1        , -1       , -1     , -1     , -1 ,
-1 , 0 , 0      , -1     , -1     };//-1 for not connected to that chip, 0 for
available, >0 means it's connected and the netNumber is stored here

// mapIJKL[]     will tell you whethrer there's a connection from that nano
pin to the corresponding special function chip
// xMapIJKL[]    will tell you the X pin that it's connected to on that sf
chip
// xStatusIJKL[] says whether that x pin is being used (this should be the
same as mt[8-10].xMap[] if theyre all stacked on top of each other)
//              I haven't decided whether to make this just a flag, or store
that signal's destination const int8_t reversePinMap[110] = {NANO_D0, NANO_D1,
NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9,
NANO_D10, NANO_D11, NANO_D12, NANO_D13, NANO_RESET, NANO_AREF, NANO_A0,
NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6,
NANO_A7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,GND,101,102,SUPPLY_3V3,104,SUPPLY_5V,DAC0,DAC1_8V,ISENSE_PLUS,ISENSE_MINUS};

};

struct netStruct net[MAX_NETS] = { //these are the special function nets that
will always be made
//netNumber,       ,netName          ,memberNodes[] ,memberBridges[][2]
,specialFunction        ,intsctNet[] ,doNotIntersectNodes[] ,priority { 127
,"Empty Net"      ,{EMPTY_NET}           ,{{}}                   ,EMPTY_NET
,{}
,{EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET} , 0},
    {     1        ,"GND\t"          ,{GND}                 ,{{}} ,GND ,{}
,{SUPPLY_3V3,SUPPLY_5V,DAC0,DAC1_8V}    , 1}, {     2        ,"+5V\t"
,{SUPPLY_5V}           ,{{}}                   ,SUPPLY_5V              ,{}
,{GND,SUPPLY_3V3,DAC0,DAC1_8V}          , 1}, {     3        ,"+3.3V\t"
,{SUPPLY_3V3}          ,{{}}                   ,SUPPLY_3V3             ,{}
,{GND,SUPPLY_5V,DAC0,DAC1_8V}           , 1}, {     4        ,"DAC 0\t"
,{DAC0}
,{{}}                   ,DAC0                ,{}
,{GND,SUPPLY_5V,SUPPLY_3V3,DAC1_8V}        , 1}, {     5        ,"DAC 1\t"
,{DAC1_8V}             ,{{}}                   ,DAC1_8V                ,{}
,{GND,SUPPLY_5V,SUPPLY_3V3,DAC0}        , 1}, {     6        ,"I Sense +"
,{ISENSE_PLUS}  ,{{}}                   ,ISENSE_PLUS     ,{} ,{ISENSE_MINUS} ,
2}, {     7        ,"I Sense -"      ,{ISENSE_MINUS} ,{{}} ,ISENSE_MINUS ,{}
,{ISENSE_PLUS}                      , 2},
};



Index   Name            Number          Nodes                   Bridges Do Not
Intersects 0       Empty Net       127             EMPTY_NET {0-0} EMPTY_NET 1
GND             1               GND,1,2,D0,3,4 {1-GND,1-2,D0-1,2-3,3-4}
3V3,5V,DAC_0,DAC_1 2       +5V             2 5V,11,12,10,9
{11-5V,11-12,10-11,9-10}        GND,3V3,DAC_0,DAC_1 3 +3.3V           3
3V3,D10,D11,D12 {D10-3V3,D10-D11,D11-D12} GND,5V,DAC_0,DAC_1 4       DAC 0 4
DAC_0 {0-0} GND,5V,3V3,DAC_1 5       DAC 1           5               DAC_1
{0-0} GND,5V,3V3,DAC_0 6       I Sense +       6 I_POS,6,5,A1,AREF
{6-I_POS,5-6,A1-5,AREF-A1}      I_NEG 7       I Sense -       7 I_NEG {0-0}
I_POS

Index   Name            Number          Nodes                   Bridges Do Not
Intersects 8       Net 8           8               7,8 {7-8} 0 9       Net 9
9               D13,D1,A7 {D13-D1,D13-A7} 0




struct chipStatus{

int chipNumber;
char chipChar;
int8_t xStatus[16]; //store the bb row or nano conn this is eventually
connected to so they can be stacked if conns are redundant int8_t yStatus[8];
//store the row/nano it's connected to const int8_t xMap[16]; const int8_t
yMap[8];

};



struct chipStatus ch[12] = {
  {0,'A',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_I, CHIP_J, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_K, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H},//X MAP constant
  {CHIP_L,  t2,t3, t4, t5, t6, t7, t8}},  // Y MAP constant

  {1,'B',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_I, CHIP_J, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_K, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,
t9,t10,t11,t12,t13,t14,t15}},

  {2,'C',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_I, CHIP_J, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_K, CHIP_H, CHIP_H}, {CHIP_L,
t16,t17,t18,t19,t20,t21,t22}},

  {3,'D',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_I, CHIP_J, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_K}, {CHIP_L,
t23,t24,t25,t26,t27,t28,t29}},

  {4,'E',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_K, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_I,
CHIP_J, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,   b2, b3,
b4, b5, b6, b7, b8}},

  {5,'F',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_K, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_I, CHIP_J, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,  b9,
b10,b11,b12,b13,b14,b15}},

  {6,'G',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_K, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_I, CHIP_J, CHIP_H, CHIP_H}, {CHIP_L,
b16,b17,b18,b19,b20,b21,b22}},

  {7,'H',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_K, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_I, CHIP_J}, {CHIP_L,
b23,b24,b25,b26,b27,b28,b29}},

  {8,'I',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_A0, NANO_D1, NANO_A2, NANO_D3, NANO_A4, NANO_D5, NANO_A6, NANO_D7,
NANO_D11, NANO_D9, NANO_D13, NANO_RESET, DAC0, ADC0, SUPPLY_3V3, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {9,'J',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_D0, NANO_A1, NANO_D2, NANO_A3, NANO_D4, NANO_A5, NANO_D6, NANO_A7,
NANO_D8, NANO_D10, NANO_D12, NANO_AREF, DAC1_8V, ADC1_5V, SUPPLY_5V, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {10,'K',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7,
NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {11,'L',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {ISENSE_MINUS, ISENSE_PLUS, ADC0, ADC1_5V, ADC2_5V, ADC3_8V, DAC1_8V, DAC0,
t1, t30, b1, b30, NANO_A4, NANO_A5, SUPPLY_5V, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}}
  };

enum nanoPinsToIndex       {     NANO_PIN_D0 ,     NANO_PIN_D1 , NANO_PIN_D2
,     NANO_PIN_D3 ,     NANO_PIN_D4 ,     NANO_PIN_D5 ,     NANO_PIN_D6 ,
NANO_PIN_D7 ,     NANO_PIN_D8 ,     NANO_PIN_D9 ,     NANO_PIN_D10 ,
NANO_PIN_D11 ,      NANO_PIN_D12 ,     NANO_PIN_D13 ,       NANO_PIN_RST ,
NANO_PIN_REF ,     NANO_PIN_A0 ,     NANO_PIN_A1 ,     NANO_PIN_A2 ,
NANO_PIN_A3 ,     NANO_PIN_A4 ,     NANO_PIN_A5 ,     NANO_PIN_A6 ,
NANO_PIN_A7 };

extern struct nanoStatus nano;


struct pathStruct{

  int node1; //these are the rows or nano header pins to connect
  int node2;
  int net;

  int chip[3];
  int x[3];
  int y[3];
  int candidates[3][3];

};
*/

