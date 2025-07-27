# PortManager

A C-based port management system that implements a ship scheduling algorithm for efficient port operations. This project demonstrates advanced scheduling concepts including priority queues, resource management, and real-time processing.

## Features

- **Ship Scheduling**: Implements a sophisticated scheduling algorithm for managing incoming and outgoing ships
- **Priority Management**: Handles emergency ships with higher priority over normal ships
- **Resource Optimization**: Efficiently manages port resources and berth allocation
- **Multiple Test Cases**: Includes comprehensive test scenarios for validation

## Project Structure

```
PortManager/
├── scheduler.c          # Main scheduling algorithm implementation
├── scheduler.out        # Compiled executable
├── validation.out       # Validation results
├── testcase1/          # Test case 1 with ship data
├── testcase2/          # Test case 2 with ship data
├── testcase3/          # Test case 3 with ship data
├── testcase4/          # Test case 4 with ship data
└── testcase5/          # Test case 5 with ship data
```

## Test Cases

Each test case directory contains:
- `emergency_ships.txt` - Emergency ship data
- `input.txt` - General input parameters
- `normal_ships.txt` - Normal ship data
- `outgoing_ships.txt` - Outgoing ship data

## Usage

1. Compile the scheduler:
   ```bash
   gcc -o scheduler.out scheduler.c
   ```

2. Run the scheduler with test data:
   ```bash
   ./validation.out
   ./scheduler.out
   ```

## Algorithm Overview

The PortManager implements a priority-based scheduling algorithm that:
- Prioritizes emergency ships over normal ships
- Manages berth allocation efficiently
- Handles ship arrival and departure timing
- Optimizes port throughput

## Technologies Used

- **Language**: C
- **Concepts**: Priority Queues, Scheduling Algorithms, Resource Management
- **Platform**: Unix/Linux/macOS compatible
