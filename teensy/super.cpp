#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "z80.h"
#include "serial.h"
#include "super.h"
#include "debug.h"
#include "rom.h"

#define SBUFLEN 80
#define SMAXARG 10
int supervisor_cmd_offset = 0;
char supervisor_cmd_buffer[SBUFLEN];
#define is_cmd(x) (!strcasecmp_P(buf, PSTR(x)))

typedef struct {
    const char *name;
    void (*function)(int argc, char *argv[]);
} cmd_entry_t;

void z80_show_regs(void);
void super_reset(int argc, char *argv[]);
void super_regs(int argc, char *argv[]);
void super_clk(int argc, char *argv[]);
void super_loadrom(int argc, char *argv[]);
void super_loadfile(int argc, char *argv[]);
void super_trace(int argc, char *argv[]);

const cmd_entry_t cmd_table[] = {
    { "quit",       NULL            },
    { "exit",       NULL            },
    { "q",          NULL            },
    { "regs",       &super_regs     },
    { "clk",        &super_clk      },
    { "clock",      &super_clk      },
    { "reset",      &super_reset    },
    { "loadrom",    &super_loadrom  },
    { "loadfile",   &super_loadfile },
    { "trace",   &super_trace },
    // list terminator:
    { NULL,         NULL            }
};

bool supervisor_menu_key_in(unsigned char keypress)
{
    if(keypress == 0x7f || keypress == 0x08){ // backspace and delete
        if(supervisor_cmd_offset > 0){
            Serial.write("\x08 \x08"); // erase last char
            supervisor_cmd_offset--;
        }
    }else if(keypress == 0x0d || keypress == 0x0a){
        supervisor_cmd_buffer[supervisor_cmd_offset] = 0;
        report("\r\n");
        execute_supervisor_command(supervisor_cmd_buffer);
        return false;
    }else if(keypress == 0x1B /* Esc */ || keypress == SUPERVISOR_ESCAPE_KEYCODE){
        report("*abort*\r\n");
        return false;
    }else if(keypress >= 0x20){
        if(supervisor_cmd_offset < (SBUFLEN-1)){
            supervisor_cmd_buffer[supervisor_cmd_offset++] = keypress;
            debug_boldon();
            Serial.write(keypress);
            debug_boldoff();
        }else{
            Serial.write(0x07); // sound the bell
        }
    }
    return true;
}

void supervisor_menu_enter(void)
{
    supervisor_cmd_offset = 0;
    report("Supervisor> ");
}

void supervisor_menu_exit(void)
{
}

bool execute_supervisor_command(char *cmd_buffer) // return false on exit/quit etc, true otherwise
{
    int argc = 0;
    char *p, *argv[SMAXARG];

    p = cmd_buffer;
    while(*p){
        while(isspace(*p)) // skip over leading whitespace
            p++;
        if(!*p) // end of line?
            break;
        argv[argc++] = p; // store ptr to start of command
        while(*p && !isspace(*p)) // find end of command
            p++;
        if(!*p) // end of line?
            break;
        *(p++) = 0; // overwrite whitespace with NUL
    }

    if(argc == 0)
        return true;

    if(!strcasecmp(argv[0], "quit") || !strcasecmp(argv[0], "exit"))
        return false;

    for(const cmd_entry_t *cmd=cmd_table; cmd->name; cmd++){
        if(!strcasecmp(argv[0], cmd->name)){
            if(cmd->function == NULL){
                return false;
            }else{
                cmd->function(argc-1, argv+1);
                return true;
            }
        }
    }

    report("error: unknown command \"%s\"\r\n", argv[0]);

    return true;
}

void super_regs(int argc, char *argv[])
{
    // TODO -- need to pause clock while we do this
    z80_show_regs();
}

void super_clk(int argc, char *argv[])
{
    float f;
    bool setclk = true;

    if(argc == 0){
        setclk = false; // we just want the report that follows
    }else if(argc == 1 && (!strcasecmp(argv[0], "stop") || !strcasecmp(argv[0], "stopped"))){
        f = 0;
    }else if(argc == 1 && !strcasecmp(argv[0], "fast")){
        f = CLK_FAST_FREQUENCY;
    }else if(argc == 1){
        char *endptr = NULL;
        f = strtof(argv[0], &endptr);
        if(f == 0 && endptr == argv[0]){
            report("error: bad frequency\r\n");
            return;
        }else{
            switch(*endptr){
                case 0:
                    break;
                case 'k':
                case 'K':
                    f *= 1000;
                    break;
                case 'm':
                case 'M':
                    f *= 1000000;
                    break;
                case 'g': // possibly getting a bit ambitious here!
                case 'G':
                    f *= 1000000000;
                    break;
                default:
                    report("error: unrecognised unit suffix?");
                    return;
            }
        }
        z80_clk_set_independent(f);
    }else{
        report("error: syntax: clk [stop|fast|<freq[kHz|MHz|GHz]>]\r\n");
        return;
    }

    if(setclk){
        if(f > CLK_SLOW_MAX_FREQUENCY && z80_bus_trace){
            report("clock: disabling bus tracing for high speed\r\n");
            z80_bus_trace = 0;
        }
        if(z80_bus_trace && f != 0.0)
            z80_clk_set_supervised(f);
        else
            z80_clk_set_independent(f);
    }

    f = z80_clk_get_frequency();
    report("clock: %s ", z80_clk_get_name());
    if(f >= 950000.0)
        report("%.3fMHz", f / 1000000.0);
    else if(f > 950.0)
        report("%.3fkHz", f / 1000.0);
    else
        report("%.3fHz", f);
    report("\r\n");
}

void super_loadrom(int argc, char *argv[])
{
    if(argc == 0){
        report("error: syntax: loadrom [basic|monitor]\r\n");
    }else if(argc == 1 && !strcasecmp(argv[0], "monitor")){
        load_program_to_sram(monitor_rom, MONITOR_ROM_START, MONITOR_ROM_SIZE, MONITOR_ROM_START);
        report("loadrom: monitor loaded at %04x\r\n", MONITOR_ROM_START);
    }else if(argc == 1 && !strcasecmp(argv[0], "basic")){
        load_program_to_sram(basic_rom, 0, 16*1024, 0);
        report("loadrom: basic loaded. entry at 0150.\r\n");
    }
}

void super_reset(int argc, char *argv[])
{
    z80_do_reset();
}


void super_loadfile(int argc, char *argv[])
{
    if(argc < 3){
        report("error: syntax: loadfile [filename] [address] [start address]\r\n");
        report("note: [filename] must be 8.3 format\r\n");
        report("note: address and start address in hex\r\n");
    }else {
      long int address = strtol(argv[1], NULL, 16);
      long int start_address = strtol(argv[2], NULL, 16);
      load_file_to_sram(argv[0], address, start_address);
    }
}

void super_trace(int argc, char *argv[])
{
    if(argc != 1){
        report("error: trace [0|1|2]\r\n");
    }else{
        z80_bus_trace = strtol(argv[0], NULL, 10);
        if(z80_bus_trace > 0)
            z80_clk_set_supervised(z80_clk_get_frequency());
    }
}
