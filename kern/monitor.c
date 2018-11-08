// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	// Pridany command na spustenie funkcie "mon_backtrace"
	{ "backtrace", "Display a backtrace", mon_backtrace },
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// Pomocne premenne na vypis + pointrovu aritmetiku.
	// uint32_t je len inak zapisany unsigned int, potrebujeme 32-bitovy, lebo
	// uz sme v chranenom mode procesora a pouzivame 32-bitovy mod
	uint32_t ebp, eip, args[5];

	// Pomocna struktura, kam budu zapisane info o funkcii, ktoru backtracujeme.
	// Zapise ich tam debuginfo_eip(uintptr_t addr, struct Eipdebuginfo *info), viac nizsie
	struct Eipdebuginfo eip_dbinfo;

	// Funkcia na zistenie ebp, vracia 32-bitovy int(nie pointer!)
	// Viac info o funkcii v inc/x86.h
	ebp = read_ebp();
	
	cprintf("Stack backtrace:\n");

	// Kedze chceme aj posledne hodnoty po tom ako sa ebp rovna null pointeru,
	// musime pouzit do/while
	do {
		// Adresa na vratenie po skonceni funkcie je prva po ebp.
		// Musime pretypovat na pointer, kedze chceme hodnotu na tejto adrese.
		// Tento "array" zapis je to iste ako *(((uint32_t *)ebp) + 1)
		eip = ((uint32_t *)ebp)[1];

		// Dalsie hodnoty na stacku po navratovej adrese su argumenty funkcie.
		// Na stack sa zapisuju opacne, cize teraz ich citame spravne, z lava do prava.
		// Nevieme presne zistit kolko ich je, lebo za nimi mozu byt lok. premenne
		// predoslej funkcie. Tak precitame 5 hodnot. Nie vsetky teda musia byt argumenty
		// backtracovanej funckie.
		args[0] = ((uint32_t *)ebp)[2];
		args[1] = ((uint32_t *)ebp)[3];
		args[2] = ((uint32_t *)ebp)[4];
		args[3] = ((uint32_t *)ebp)[5];
		args[4] = ((uint32_t *)ebp)[6];

		// Vypis ebp, eip a 5 argumentov.
		// %08x vypise hexadecimalne cislo, ktore ma 8 cislic, ak nema tak doplni nulami.
		// Kedze su uz tieto premenne pointre, nemusime do printu pred ne dat '&'.
		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n",
			ebp, eip, args[0], args[1], args[2], args[3], args[4]);

		// Ak bola adresa v premennej eip spravna, tak debuginfo_eip vrati 0, inak -1
		// Zisti info o funkcii, do ktorej ukazuje adresa ulozena v eip a hodi to info
		// do eip_dbinfo. Viac info o funkcii v kern/kdebug.c
		if(!debuginfo_eip(eip, &eip_dbinfo))
		{
			cprintf("         %s:%d: %.*s+%d\n",
				eip_dbinfo.eip_file, eip_dbinfo.eip_line,
				eip_dbinfo.eip_fn_namelen, eip_dbinfo.eip_fn_name,
				eip - eip_dbinfo.eip_fn_addr);
		}

		// V ebp je ulozena adresa predosleho ebp, cize len dereferencujeme
		// a tym sa posunieme na dalsie ebp
		ebp = *((uint32_t *) ebp);

	// Ak narazime na ebp rovne null pointeru, prebehne posledny
	// cyklus, aby sa vypisala posledna funkcia v ktorej sa nachadzame
	} while(ebp);
	return 0;
}



/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("\x1b[36mWelcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
