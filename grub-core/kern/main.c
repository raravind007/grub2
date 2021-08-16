/* main.c - the kernel main routine */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2003,2005,2006,2008,2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/kernel.h>
#include <grub/misc.h>
#include <grub/symbol.h>
#include <grub/dl.h>
#include <grub/term.h>
#include <grub/file.h>
#include <grub/device.h>
#include <grub/env.h>
#include <grub/mm.h>
#include <grub/command.h>
#include <grub/reader.h>
#include <grub/parser.h>

#ifdef GRUB_MACHINE_PCBIOS
#include <grub/machine/memory.h>
#endif

grub_addr_t
grub_modules_get_end (void)
{
  struct grub_module_info *modinfo;

  modinfo = (struct grub_module_info *) grub_modbase;

  /* Check if there are any modules.  */
  if ((modinfo == 0) || modinfo->magic != GRUB_MODULE_MAGIC)
    return grub_modbase;

  return grub_modbase + modinfo->size;
}

/* Load all modules in core.  */
static void
grub_load_modules (void)
{
  struct grub_module_header *header;
  FOR_MODULES (header)
  {
    /* Not an ELF module, skip.  */
    if (header->type != OBJ_TYPE_ELF)
      continue;

    if (! grub_dl_load_core ((char *) header + sizeof (struct grub_module_header),
			     (header->size - sizeof (struct grub_module_header))))
      grub_fatal ("%s", grub_errmsg);

    if (grub_errno)
      grub_print_error ();
  }
}

static char *load_config;

static void
grub_load_config (void)
{
  struct grub_module_header *header;
  FOR_MODULES (header)
  {
    /* Not an embedded config, skip.  */
    if (header->type != OBJ_TYPE_CONFIG)
      continue;

    load_config = grub_malloc (header->size - sizeof (struct grub_module_header) + 1);
    if (!load_config)
      {
	grub_print_error ();
	break;
      }
    grub_memcpy (load_config, (char *) header +
		 sizeof (struct grub_module_header),
		 header->size - sizeof (struct grub_module_header));
    load_config[header->size - sizeof (struct grub_module_header)] = 0;
    break;
  }
}

/* Write hook for the environment variables of root. Remove surrounding
   parentheses, if any.  */
static char *
grub_env_write_root (struct grub_env_var *var __attribute__ ((unused)),
		     const char *val)
{
  /* XXX Is it better to check the existence of the device?  */
  grub_size_t len = grub_strlen (val);

  if (val[0] == '(' && val[len - 1] == ')')
    return grub_strndup (val + 1, len - 2);

  return grub_strdup (val);
}

static void
grub_set_prefix_and_root (void)
{
  char *device = NULL;
  char *path = NULL;
  char *fwdevice = NULL;
  char *fwpath = NULL;
  char *prefix = NULL;
  struct grub_module_header *header;

  FOR_MODULES (header)
    if (header->type == OBJ_TYPE_PREFIX)
      prefix = (char *) header + sizeof (struct grub_module_header);

  grub_register_variable_hook ("root", 0, grub_env_write_root);

  grub_machine_get_bootlocation (&fwdevice, &fwpath);

  if (fwdevice && fwpath)
    {
      char *fw_path;
      char separator[3] = ")";

      grub_dprintf ("fw_path", "\n");
      grub_dprintf ("fw_path", "fwdevice:\"%s\" fwpath:\"%s\"\n", fwdevice, fwpath);

      if (!grub_strncmp(fwdevice, "http", 4) && fwpath[0] != '/')
	grub_strcpy(separator, ")/");

      fw_path = grub_xasprintf ("(%s%s%s", fwdevice, separator, fwpath);
      if (fw_path)
	{
	  grub_env_set ("fw_path", fw_path);
	  grub_env_export ("fw_path");
	  grub_dprintf ("fw_path", "fw_path:\"%s\"\n", fw_path);
	  grub_free (fw_path);
	}
    }

  if (prefix)
    {
      char *pptr = NULL;
      if (prefix[0] == '(')
	{
	  pptr = grub_strrchr (prefix, ')');
	  if (pptr)
	    {
	      device = grub_strndup (prefix + 1, pptr - prefix - 1);
	      pptr++;
	    }
	}
      if (!pptr)
	pptr = prefix;
      if (pptr[0])
	path = grub_strdup (pptr);
    }

  if (!device && fwdevice)
    device = fwdevice;
  else if (fwdevice && (device[0] == ',' || !device[0]))
    {
      /* We have a partition, but still need to fill in the drive.  */
      char *comma, *new_device;

      for (comma = fwdevice; *comma; )
	{
	  if (comma[0] == '\\' && comma[1] == ',')
	    {
	      comma += 2;
	      continue;
	    }
	  if (*comma == ',')
	    break;
	  comma++;
	}
      if (*comma)
	{
	  char *drive = grub_strndup (fwdevice, comma - fwdevice);
	  new_device = grub_xasprintf ("%s%s", drive, device);
	  grub_free (drive);
	}
      else
	new_device = grub_xasprintf ("%s%s", fwdevice, device);

      grub_free (fwdevice);
      grub_free (device);
      device = new_device;
    }
  else
    grub_free (fwdevice);
  if (fwpath && !path)
    {
      grub_size_t len = grub_strlen (fwpath);
      while (len > 1 && fwpath[len - 1] == '/')
	fwpath[--len] = 0;
      if (len >= sizeof (GRUB_TARGET_CPU "-" GRUB_PLATFORM) - 1
	  && grub_memcmp (fwpath + len - (sizeof (GRUB_TARGET_CPU "-" GRUB_PLATFORM) - 1), GRUB_TARGET_CPU "-" GRUB_PLATFORM,
			  sizeof (GRUB_TARGET_CPU "-" GRUB_PLATFORM) - 1) == 0)
	fwpath[len - (sizeof (GRUB_TARGET_CPU "-" GRUB_PLATFORM) - 1)] = 0;
      path = fwpath;
    }
  else
    grub_free (fwpath);
  if (device)
    {
      char *prefix_set;

#ifdef __powerpc__
      /* We have to be careful here on powerpc-ieee1275 + signed grub. We
	 will have signed something with a prefix that doesn't have a device
	 because we cannot know in advance what partition we're on.

	 We will have had !device earlier, so we will have set device=fwdevice
	 However, we want to make sure we do not end up setting prefix to be
	 ($fwdevice)/path, because we will then end up trying to boot or search
	 based on a prefix of (ieee1275/disk)/path, which will not work because
	 it's missing a partition.

	 Also:
	  - You can end up with a device with an FS directly on it, without
	    a partition, e.g. ieee1275/cdrom.

	  - powerpc-ieee1275 + grub-install sets e.g. prefix=(,gpt2)/path,
	    which will have now been extended to device=$fwdisk,partition
	    and path=/path

	  - PowerVM will give us device names like
	    ieee1275//vdevice/v-scsi@3000006c/disk@8100000000000000
	    and we don't want to try to encode some sort of truth table about
	    what sorts of paths represent disks with partition tables and those
	    without partition tables.

	 So we act unless there is a comma in the device, which would indicate
	 a partition has already been specified.

	 (If we only have a path, the code in normal to discover config files
	 will try both without partitions and then with any partitions so we
	 will cover both CDs and HDs.)
       */
      if (grub_strchr (device, ',') == NULL)
        grub_env_set ("prefix", path);
      else
#endif
	{
	  prefix_set = grub_xasprintf ("(%s)%s", device, path ? : "");
	  if (prefix_set)
	  {
	    grub_env_set ("prefix", prefix_set);
	    grub_free (prefix_set);
	  }
	}

      grub_env_set ("root", device);
    }

  grub_free (device);
  grub_free (path);
  grub_print_error ();
}

/* Load the normal mode module and execute the normal mode if possible.  */
static void
grub_load_normal_mode (void)
{
  /* Load the module.  */
  grub_dl_load ("normal");

  /* Print errors if any.  */
  grub_print_error ();
  grub_errno = 0;

  grub_command_execute ("normal", 0, 0);
}

static void
reclaim_module_space (void)
{
  grub_addr_t modstart, modend;

  if (!grub_modbase)
    return;

#ifdef GRUB_MACHINE_PCBIOS
  modstart = GRUB_MEMORY_MACHINE_DECOMPRESSION_ADDR;
#else
  modstart = grub_modbase;
#endif
  modend = grub_modules_get_end ();
  grub_modbase = 0;

#if GRUB_KERNEL_PRELOAD_SPACE_REUSABLE
  grub_mm_init_region ((void *) modstart, modend - modstart);
#else
  (void) modstart;
  (void) modend;
#endif
}

/* The main routine.  */
void __attribute__ ((noreturn))
grub_main (void)
{
  /* First of all, initialize the machine.  */
  grub_machine_init ();

  grub_boot_time ("After machine init.");

  grub_load_config ();

  grub_boot_time ("Before loading embedded modules.");

  /* Load pre-loaded modules and free the space.  */
  grub_register_exported_symbols ();
#ifdef GRUB_LINKER_HAVE_INIT
  grub_arch_dl_init_linker ();
#endif  
  grub_load_modules ();

  grub_boot_time ("After loading embedded modules.");

  /* It is better to set the root device as soon as possible,
     for convenience.  */
  grub_set_prefix_and_root ();
  grub_env_export ("root");
  grub_env_export ("prefix");

  /* Reclaim space used for modules.  */
  reclaim_module_space ();

  grub_boot_time ("After reclaiming module space.");

  grub_register_core_commands ();

  grub_boot_time ("Before execution of embedded config.");

  if (load_config)
    grub_parser_execute (load_config);

  grub_boot_time ("After execution of embedded config. Attempt to go to normal mode");

  grub_load_normal_mode ();
  grub_rescue_run ();
}
