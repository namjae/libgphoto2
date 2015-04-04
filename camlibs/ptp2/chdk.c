/* chdk.c
 *
 * Copyright (C) 2015 Marcus Meissner <marcus@jet.franken.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "config.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include <gphoto2/gphoto2-library.h>
#include <gphoto2/gphoto2-port-log.h>
#include <gphoto2/gphoto2-setting.h>

#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (GETTEXT_PACKAGE, String)
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  define textdomain(String) (String)
#  define gettext(String) (String)
#  define dgettext(Domain,Message) (Message)
#  define dcgettext(Domain,Message,Type) (Message)
#  define bindtextdomain(Domain,Directory) (Domain)
#  define _(String) (String)
#  define N_(String) (String)
#endif

#include "ptp.h"
#include "ptp-bugs.h"
#include "ptp-private.h"
#include "ptp-pack.c"

/* CHDK support */

/* Include the rlibs.lua lua libs or try do it on our own? */

/* capture: 
 * send LUA: 
 * init_usb_capture(0); is exiting capture

 * init_usb_capture(1,0,0)  1 = jpeg, 2 = raw, 4 = switch raw to dng
 *
 * ff_imglist(options)

 * return ls('path'[,'opts')

 * return ff_download(srcpaths[,ropts])
 *
 * return ff_mdelete(paths)
 * 
 * return os.stat(path)
 * return os.utime(path)
 * return os.mkdir(path)
 * return os.remove(path)
 * return mkdir_m(path(s))

 * rs_init for starting usb shootinp
 */

static int
chdk_generic_script_run (
	PTPParams *params, const char *luascript,
	char **table, int *retint, GPContext *context
) {
	int 			scriptid = 0;
	unsigned int		status;
	int			luastatus;
	ptp_chdk_script_msg	*msg = NULL;
	char			*xtable = NULL;
	int			xint = -1;

	if (!table) table = &xtable;
	if (!retint) retint = &xint;

	GP_LOG_D ("calling lua script %s", luascript);
	C_PTP (ptp_chdk_exec_lua(params, (char*)luascript, 0, &scriptid, &luastatus));
	GP_LOG_D ("called script. script id %d, status %d", scriptid, luastatus);

	*table = NULL;
	*retint = -1;

	while (1) {
		C_PTP (ptp_chdk_get_script_status(params, &status));
		GP_LOG_D ("script status %x", status);

		if (status & PTP_CHDK_SCRIPT_STATUS_MSG) {
			C_PTP (ptp_chdk_read_script_msg(params, &msg));
			GP_LOG_D ("message script id %d, type %d, subtype %d", msg->script_id, msg->type, msg->subtype);
			switch (msg->type) {
			case PTP_CHDK_S_MSGTYPE_RET:
			case PTP_CHDK_S_MSGTYPE_USER:
				switch (msg->subtype) {
				case PTP_CHDK_TYPE_UNSUPPORTED: GP_LOG_D("unsupported");break;
				case PTP_CHDK_TYPE_NIL: GP_LOG_D("nil");break;
				case PTP_CHDK_TYPE_BOOLEAN:
					*retint = msg->data[0]; /* 3 more bytes, but rest are not interestng for bool */
					GP_LOG_D("boolean %d", *retint);
					break;
				case PTP_CHDK_TYPE_INTEGER:
					GP_LOG_D("int %02x %02x %02x %02x", msg->data[0], msg->data[1], msg->data[2], msg->data[3]);
					*retint = le32atoh((unsigned char*)msg->data);
					break;
				case PTP_CHDK_TYPE_STRING: GP_LOG_D("string %s", msg->data);break;
				case PTP_CHDK_TYPE_TABLE:
					GP_LOG_D("table %s", msg->data);
					if (*table) {
						*table = realloc(*table,strlen(*table)+strlen(msg->data)+1);
						strcat(*table,msg->data);
					} else { 
						*table = strdup(msg->data);
					}
					break;
				default: GP_LOG_E("unknown chdk msg->type %d", msg->subtype);break;
				}
				break;
			case PTP_CHDK_S_MSGTYPE_ERR:
				GP_LOG_D ("error %d, message %s", msg->subtype, msg->data);
				break;
			default:
				GP_LOG_E ("unknown msg->type %d", msg->type);
				break;
			}
			free (msg);
		}

		if (!status) /* this means we read out all messages */
			break;

		/* wait for script to finish */
		if (status & PTP_CHDK_SCRIPT_STATUS_RUN)
			usleep(100*1000); /* 100 ms */
	}
	if (xtable)
		GP_LOG_E("a string return was unexpected, returned value: %s", xtable);
	if (xint != -1)
		GP_LOG_E("a int return was unexpected, returned value: %d", xint);
	return GP_OK;
}


static int
chdk_list_func (CameraFilesystem *fs, const char *folder, CameraList *list,
                void *data, GPContext *context, int dirsonly)
{
	Camera *camera = (Camera *)data;
	PTPParams		*params = &camera->pl->params;
	int			retint = FALSE;
	int			ret;
	int			tablecnt;
	/* return ls("A/path/"); */
	const char 		*luascript = PTP_CHDK_LUA_LS"\nreturn ls('A%s',{\nstat='*',\n})";
	char 			*lua = NULL;
	char			*t, *table = NULL;
	char			*xfolder;

	/* strip leading / of folders, except for the root folder */
	xfolder=strdup(folder);
	if (strlen(folder)>2 && (xfolder[strlen(xfolder)-1] == '/'))
		xfolder[strlen(xfolder)-1] = '\0';

	C_MEM (lua = malloc(strlen(luascript)+strlen(xfolder)+1));

	sprintf(lua,luascript,xfolder);
	free(xfolder);

	ret = chdk_generic_script_run (params, lua, &table, &retint, context);
	if (ret != GP_OK)
		return ret;
	if (table) {
		t = table;
	/* table {[1]="NIKON001.DSC",[2]="DCIM",[3]="MISC",[4]="DISKBOOT.BIN",[5]="CHDK",[6]="changelog.txt",[7]="vers.req",[8]="readme.txt",[9]="PS.FI2",} */

	/* table {[1]={is_file=true,mtime=1402161416,name="NIKON001.DSC",ctime=1402161416,attrib=34,is_dir=false,size=512,},[2]={is_file=false,mtime=1402161416,name="DCIM",ctime=1402161416,attrib=16,is_dir=true,size=0,},[3]={is_file=false,mtime=1406460938,name="MISC",ctime=1406460938,attrib=16,is_dir=true,size=0,},[4]={is_file=true,mtime=1398564982,name="DISKBOOT.BIN",ctime=1423347896,attrib=32,is_dir=false,size=138921,},[5]={is_file=false,mtime=1423347900,name="CHDK",ctime=1423347900,attrib=16,is_dir=true,size=0,},[6]={is_file=true,mtime=1395026402,name="changelog.txt",ctime=1423347900,attrib=32,is_dir=false,size=2093,},[7]={is_file=true,mtime=1217623956,name="vers.req",ctime=1423347900,attrib=32,is_dir=false,size=107,},[8]={is_file=true,mtime=1398564982,name="readme.txt",ctime=1423347900,attrib=32,is_dir=false,size=10518,},[9]={is_file=true,mtime=1398564982,name="PS.FI2",ctime=1423347900,attrib=32,is_dir=false,size=80912,},}
*/

nexttable:
		if (*t != '{')
			return GP_ERROR;
		t++;
		tablecnt = 0;
		while (*t) {
			int cnt;
			char *name = NULL;
			int isfile = 0, mtime = 0, attrib = -1, ctime = 0, size = -1;
			CameraFileInfo info;

			if (*t++ != '[') {
				GP_LOG_E("expected [, have %c", t[-1]);
				break;
			}
			if (!sscanf(t,"%d",&cnt)) {
				GP_LOG_E("expected integer");
				break;
			}
			GP_LOG_D("parsing entry %d", cnt);
			if (cnt != tablecnt + 1) {
				GP_LOG_E("cnt %d, tablecnt %d, expected %d", cnt, tablecnt, tablecnt+1);
				break;
			}
			tablecnt++;
			t = strchr(t,']');
			if (!t) {
				GP_LOG_E("expected ]");
				break;
			}
			t++;
			if (*t++ != '=') {
				GP_LOG_E("expected =");
				break;
			}
			/* {is_file=true,mtime=1402161416,name="NIKON001.DSC",ctime=1402161416,attrib=34,is_dir=false,size=512,} */
			if (*t++ != '{') {
				GP_LOG_E("expected {");
				break;
			}
			memset(&info,0,sizeof(info));
			while (*t && *t != '}') {
				/* GP_LOG_D("parsing %s", t); */
				if (t==strstr(t,"is_file=true")) { isfile = TRUE; }
				if (t==strstr(t,"is_file=false")) { isfile = FALSE; }
				if (t==strstr(t,"is_dir=true")) { isfile = FALSE; }
				if (t==strstr(t,"is_dir=false")) { isfile = TRUE; }
				if (t==strstr(t,"name=\"")) {
					char *s;
					name = t+strlen("name=.");
					s = strchr(name,'"');
					if (s) *s='\0';
					name = strdup(name);
					GP_LOG_D("name is %s", name);
					*s = '"';
				}
				if (sscanf(t,"mtime=%d,", &mtime)) {
					info.file.mtime = mtime;
					info.file.fields |= GP_FILE_INFO_MTIME;
				}
				if (sscanf(t,"size=%d,", &size)) {
					info.file.size = size;
					info.file.fields |= GP_FILE_INFO_SIZE;
				}
				if (!sscanf(t,"ctime=%d,", &ctime)) { }
				if (!sscanf(t,"attrib=%d,", &attrib)) { }
				t = strchr(t,',');
				if (t) t++;
			}
			if (*t)
				t++;
			/* Directories: return as list. */
			if (dirsonly && !isfile)
				gp_list_append (list, name, NULL);

			/* Files: Add directly to FS, including the meta info too. */
			if (!dirsonly && isfile) {
				gp_filesystem_append(fs, folder, name, context);
				gp_filesystem_set_info_noop(fs, folder, name, info, context);
			}
			free(name);

			if (*t++ != ',') {
				GP_LOG_E("expected , got %c", t[-1]);
				break;
			}
			if (*t == '}') { t++; break; }
		}
		if (*t) {
			if (*t == '{') goto nexttable;
			GP_LOG_E("expected end of string or { , got %s", t);
			return GP_ERROR;
		}
		free (table);
		table = NULL;
	}
	if (retint)
		return GP_OK;
	GP_LOG_E("boolean return from LUA ls was %d", retint);
	return GP_ERROR;
}

static int
chdk_file_list_func (CameraFilesystem *fs, const char *folder, CameraList *list,
                void *data, GPContext *context)
{
	return chdk_list_func(fs,folder,list,data,context,FALSE);
}


static int
chdk_folder_list_func (CameraFilesystem *fs, const char *folder, CameraList *list,
                  void *data, GPContext *context)
{
	return chdk_list_func(fs,folder,list,data,context,TRUE);
}

static int
chdk_get_info_func (CameraFilesystem *fs, const char *folder, const char *filename,
               CameraFileInfo *info, void *data, GPContext *context)
{
	Camera *camera = (Camera *)data;
	PTPParams		*params = &camera->pl->params;
	int			retint = 0;
	int			ret;
	char			*table = NULL;
	const char 		*luascript = "\nreturn os.stat('A%s/%s')";
	char 			*lua = NULL;

	C_MEM (lua = malloc(strlen(luascript)+strlen(folder)+strlen(filename)+1));
	sprintf(lua,luascript,folder,filename);
	ret = chdk_generic_script_run (params, lua, &table, &retint, context);
	free (lua);
	if (table) {
		char *t = table;
		int x;
		while (*t) {
			if (sscanf(t,"mtime %d", &x)) {
				info->file.fields |= GP_FILE_INFO_MTIME;
				info->file.mtime = x;
			}
			if (sscanf(t,"size %d", &x)) {
				info->file.fields |= GP_FILE_INFO_SIZE;
				info->file.size = x;
			}
			t = strchr(t,'\n');
			if (t) t++;
		}
		free (table);
	}
	return ret;
}

static int
chdk_delete_file_func (CameraFilesystem *fs, const char *folder,
                        const char *filename, void *data, GPContext *context)
{
	Camera *camera = (Camera *)data;
	PTPParams		*params = &camera->pl->params;
	int			ret;
	const char 		*luascript = "\nreturn os.remove('A%s/%s')";
	char 			*lua = NULL;

	C_MEM (lua = malloc(strlen(luascript)+strlen(folder)+strlen(filename)+1));
	sprintf(lua,luascript,folder,filename);
	ret = chdk_generic_script_run (params, lua, NULL, NULL, context);
	free (lua);
	return ret;
}

static int
chdk_get_file_func (CameraFilesystem *fs, const char *folder, const char *filename,
               CameraFileType type, CameraFile *file, void *data,
               GPContext *context)
{
        Camera 			*camera = data;
	PTPParams		*params = &camera->pl->params;
	uint16_t        	ret;
	PTPDataHandler  	handler;
	char 			*fn;

	fn = malloc(1+strlen(folder)+1+strlen(filename)+1),
	sprintf(fn,"A%s/%s",folder,filename);
	ptp_init_camerafile_handler (&handler, file);
	ret = ptp_chdk_download(params, fn, &handler);
	free (fn);
	ptp_exit_camerafile_handler (&handler);
	if (ret == PTP_ERROR_CANCEL)
		return GP_ERROR_CANCEL;
	C_PTP_REP (ret);
	return GP_OK;
}


static CameraFilesystemFuncs chdk_fsfuncs = {
	.file_list_func         = chdk_file_list_func,
	.folder_list_func       = chdk_folder_list_func,
	.get_info_func          = chdk_get_info_func,
	.get_file_func          = chdk_get_file_func,
	.del_file_func          = chdk_delete_file_func,
/*
	.set_info_func          = chdk_set_info_func,
	.read_file_func         = chdk_read_file_func,
	.put_file_func          = chdk_put_file_func,
	.make_dir_func          = chdk_make_dir_func,
	.remove_dir_func        = chdk_remove_dir_func,
	.storage_info_func      = chdk_storage_info_func
*/
};

static int
camera_prepare_chdk_capture(Camera *camera, GPContext *context) {
	PTPParams		*params = &camera->pl->params;
	int			ret = 0, retint = 0;
	char			*table = NULL;
	char			*lua =
PTP_CHDK_LUA_SERIALIZE
"if not get_mode() then\n\
	switch_mode_usb(1)\n\
	local i=0\n\
	while not get_mode() and i < 300 do\n\
		sleep(10)\n\
		i=i+1\n\
	end\n\
	if not get_mode() then\n\
		return false, 'switch failed'\n\
	end\n\
	return true\n\
end\n\
return false,'already in rec'\n\
";

	ret = chdk_generic_script_run (params, lua, &table, &retint, context);
	if(table) GP_LOG_D("table returned: %s\n", table);
	free(table);
	return ret;
}

static int
camera_unprepare_chdk_capture(Camera *camera, GPContext *context) {
	PTPParams		*params = &camera->pl->params;
	int 			ret, retint;
	char			*table;
	char			*lua =
PTP_CHDK_LUA_SERIALIZE
"if get_mode() then\n\
	switch_mode_usb(0)\n\
	local i=0\n\
	while get_mode() and i < 300 do\n\
		sleep(10)\n\
		i=i+1\n\
	end\n\
	if get_mode() then\n\
		return false, 'switch failed'\n\
	end\n\
	return true\n\
end\n\
return false,'already in play'\n\
";
	ret = chdk_generic_script_run (params, lua, &table, &retint, context);
	if(table) GP_LOG_D("table returned: %s\n", table);
	free(table);

	return ret;
}


static int
chdk_camera_about (Camera *camera, CameraText *text, GPContext *context)
{
        snprintf (text->text, sizeof(text->text),
         _("PTP2 / CHDK driver\n"
           "(c) 2015-%d by Marcus Meissner <marcus@jet.franken.de>.\n"
           "This is a PTP subdriver that supports CHDK using Canon cameras.\n"
           "\n"
           "Enjoy!"), 2015);
        return GP_OK;
}

static int
chdk_camera_summary (Camera *camera, CameraText *text, GPContext *context)
{
	PTPParams	*params = &camera->pl->params;
	int 		ret;
	char 		*s = text->text;
	int		major, minor, retint;

	C_PTP (ptp_chdk_get_version (params, &major, &minor));
        sprintf (s, _("CHDK %d.%d Status:\n"), major, minor ); s += strlen(s);

	ret = chdk_generic_script_run (params, "return get_mode()", NULL, &retint, context);
	sprintf (s, _("Mode: %d\n"), retint); s += strlen(s);
	ret = chdk_generic_script_run (params, "return get_sv96()", NULL, &retint, context);
	sprintf (s, _("SV96: %d, ISO: %d\n"), retint, (int)(exp2(retint/96.0)*3.125)); s += strlen(s);
	ret = chdk_generic_script_run (params, "return get_tv96()", NULL, &retint, context);
	sprintf (s, _("TV96: %d, Shutterspeed: %f\n"), retint, 1.0/exp2(retint/96.0)); s += strlen(s);
	ret = chdk_generic_script_run (params, "return get_av96()", NULL, &retint, context);
	sprintf (s, _("AV96: %d, Aperture: %f\n"), retint, sqrt(exp2(retint/96.0))); s += strlen(s);
	ret = chdk_generic_script_run (params, "return get_focus()", NULL, &retint, context);
	sprintf (s, _("Focus: %d\n"), retint); s += strlen(s);
	ret = chdk_generic_script_run (params, "return get_iso_mode()", NULL, &retint, context);
	sprintf (s, _("ISO Mode: %d\n"), retint); s += strlen(s);

	ret = chdk_generic_script_run (params, "return get_zoom()", NULL, &retint, context);
	sprintf (s, _("Zoom: %d\n"), retint); s += strlen(s);
        return ret;
/* 
Mode: 256
SV96: 603, ISO: 243
TV96: 478, Shutterspeed: 0
AV96: 294, Aperture: 2,890362
ND Filter: -1
Focus: 166
ISO Mode: 0

*/

}

struct submenu;
typedef int (*get_func) (PTPParams *, struct submenu*, CameraWidget **, GPContext *);
#define CONFIG_GET_ARGS PTPParams *params, struct submenu *menu, CameraWidget **widget, GPContext *context
typedef int (*put_func) (PTPParams *, CameraWidget *, GPContext *);
#define CONFIG_PUT_ARGS PTPParams *params, CameraWidget *widget, GPContext *context

struct submenu {
	char		*label;
	char		*name;
        get_func	getfunc;
        put_func	putfunc;
};

static int
chdk_get_iso(CONFIG_GET_ARGS) {
	int retint = 0, iso = 0;
	char buf[20];

	CR (chdk_generic_script_run (params, "return get_iso_mode()", NULL, &retint, context));
	if (!retint) {
		CR(chdk_generic_script_run (params, "return get_sv96()", NULL, &retint, context));
		iso = (int)(exp2(retint/96.0)*3.125);
	} else {
		iso = retint;
	}
	CR (gp_widget_new (GP_WIDGET_TEXT, _(menu->label), widget));
	gp_widget_set_name (*widget, menu->name);
	sprintf(buf,"%d", iso);
	gp_widget_set_value (*widget, buf);
	return GP_OK;
}

static int
chdk_put_iso(CONFIG_PUT_ARGS) {
	int iso = 0;
	char *val;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (!sscanf(val, "%d", &iso))
		return GP_ERROR_BAD_PARAMETERS;

	sprintf(lua,"return set_iso_mode(%d)\n", iso);
	CR (chdk_generic_script_run (params, lua, NULL, NULL, context));
	return GP_OK;
}

static int
chdk_get_av(CONFIG_GET_ARGS) {
	int retint = 0;
	char buf[20];
	float f;

	CR (chdk_generic_script_run (params, "return get_av96()", NULL, &retint, context));
	f = sqrt(exp2(retint/96.0));
	CR (gp_widget_new (GP_WIDGET_TEXT, _(menu->label), widget));
	gp_widget_set_name (*widget, menu->name);
	sprintf(buf, "%d.%d", (int)f,((int)f*10)%10);
	gp_widget_set_value (*widget, buf);
	return GP_OK;
}

static int
chdk_put_av(CONFIG_PUT_ARGS) {
	char *val;
	int av1,av2,sqav;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (2 != sscanf(val, "%d.%d", &av1,&av2)) {
		if (!sscanf(val, "%d", &av1)) {
			return GP_ERROR_BAD_PARAMETERS;
		}
		av2 = 0;
	}

	/* av96 = 96*log2(f^2) */
	sqav = (av1*1.0+av2/10.0)*(av1*1.0+av2/10.0);
	sprintf(lua,"return set_av96(%d)\n", (int)(96.0*log2(sqav)));
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_tv(CONFIG_GET_ARGS) {
	int retint = 0;
	char buf[20];

	CR (chdk_generic_script_run (params, "return get_tv96()", NULL, &retint, context));
	CR (gp_widget_new (GP_WIDGET_TEXT, _(menu->label), widget));
	gp_widget_set_name (*widget, menu->name);
	sprintf(buf, "%f", 1.0/exp2(retint/96.0));
	gp_widget_set_value (*widget, buf);
	return GP_OK;
}

static int
chdk_put_tv(CONFIG_PUT_ARGS) {
	char *val;
	float	f;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (!sscanf(val, "%f", &f))
		return GP_ERROR_BAD_PARAMETERS;

	sprintf(lua,"return set_tv96(%d)\n", (int)(96.0*(-log2(f))));
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_focus(CONFIG_GET_ARGS) {
	int retint = 0;
	char buf[20];

	CR (chdk_generic_script_run (params, "return get_focus()", NULL, &retint, context));
	CR (gp_widget_new (GP_WIDGET_TEXT, _(menu->label), widget));
	sprintf(buf, "%dmm", retint);
	gp_widget_set_value (*widget, buf);
	return GP_OK;
}

static int
chdk_put_focus(CONFIG_PUT_ARGS) {
	char *val;
	int focus;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (!sscanf(val, "%dmm", &focus))
		return GP_ERROR_BAD_PARAMETERS;

	sprintf(lua,"return set_focus(%d)\n", focus);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_zoom(CONFIG_GET_ARGS) {
	int retint = 0;
	char buf[20];

	CR (chdk_generic_script_run (params, "return get_zoom()", NULL, &retint, context));
	CR (gp_widget_new (GP_WIDGET_TEXT, _(menu->label), widget));
	sprintf(buf, "%d", retint);
	gp_widget_set_value (*widget, buf);
	return GP_OK;
}

static int
chdk_put_zoom(CONFIG_PUT_ARGS) {
	char *val;
	int zoom;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (!sscanf(val, "%d", &zoom))
		return GP_ERROR_BAD_PARAMETERS;

	sprintf(lua,"return set_zoom(%d)\n", zoom);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static void
add_buttons(CameraWidget *widget) {
	gp_widget_add_choice(widget, "shoot_half");
	gp_widget_add_choice(widget, "shoot_full");
	gp_widget_add_choice(widget, "shoot_full_only");
}

static int
chdk_get_press(CONFIG_GET_ARGS) {
	CR (gp_widget_new (GP_WIDGET_RADIO, _(menu->label), widget));
	gp_widget_set_value (*widget, "chdk buttonname");
	add_buttons(*widget);
	return GP_OK;
}

static int
chdk_put_press(CONFIG_PUT_ARGS) {
	char *val;
	char lua[100];

	gp_widget_get_value (widget, &val);
	sprintf(lua,"press('%s')\n", val);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_release(CONFIG_GET_ARGS) {
	CR (gp_widget_new (GP_WIDGET_RADIO, _(menu->label), widget));
	gp_widget_set_value (*widget, "chdk buttonname");
	add_buttons(*widget);
	return GP_OK;
}

static int
chdk_put_release(CONFIG_PUT_ARGS) {
	char *val;
	char lua[100];

	gp_widget_get_value (widget, &val);
	sprintf(lua,"release('%s')\n", val);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_click(CONFIG_GET_ARGS) {
	CR (gp_widget_new (GP_WIDGET_RADIO, _(menu->label), widget));
	gp_widget_set_value (*widget, "chdk buttonname");
	gp_widget_add_choice(*widget, "erase");
	gp_widget_add_choice(*widget, "up");
	gp_widget_add_choice(*widget, "print");
	gp_widget_add_choice(*widget, "left");
	gp_widget_add_choice(*widget, "set");
	gp_widget_add_choice(*widget, "right");
	gp_widget_add_choice(*widget, "disp");
	gp_widget_add_choice(*widget, "down");
	gp_widget_add_choice(*widget, "menu");
	gp_widget_add_choice(*widget, "zoom_in");
	gp_widget_add_choice(*widget, "zoom_out");
	gp_widget_add_choice(*widget, "video");
	gp_widget_add_choice(*widget, "shoot_full");
	gp_widget_add_choice(*widget, "shoot_full_only");
	return GP_OK;
}

static int
chdk_put_click(CONFIG_PUT_ARGS) {
	char *val;
	char lua[100];

	gp_widget_get_value (widget, &val);
	if (!strcmp(val,"wheel l"))
		strcpy(lua,"post_levent_to_ui(\"RotateJogDialLeft\",1)\n");
	else if (!strcmp(val,"wheel r"))
		strcpy(lua,"post_levent_to_ui(\"RotateJogDialRight\",1)\n");
	else
		sprintf(lua,"click('%s')\n", val);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

static int
chdk_get_capmode(CONFIG_GET_ARGS) {
	char *table = NULL;
	int retint = 0;
	char *lua;
	const char *luascript = 
PTP_CHDK_LUA_SERIALIZE \
"capmode=require'capmode'\n"
"local l={}\n"
"local i=1\n"
"for id,name in ipairs(capmode.mode_to_name) do\n"
"	if capmode.valid(id) then\n"
"		l[i] = {name=name,id=id}\n"
"		i = i + 1\n"
"	end\n"
"end\n"
"return serialize(l),capmode.get()\n";

	lua = malloc(strlen(luascript));
	sprintf(lua,luascript); /* changes the %% in the serializer to % */

	CR (chdk_generic_script_run (params,lua,&table,&retint,context));

	/* {[1]={name="AUTO",id=1,},[2]={name="P",id=2,},[3]={name="PORTRAIT",id=6,},[4]={name="NIGHT_SCENE",id=7,},[5]={name="SCN_UNDERWATER",id=17,},[6]={name="LONG_SHUTTER",id=19,},[7]={name="SCN_BEACH",id=23,},[8]={name="SCN_FIREWORK",id=24,},[9]={name="SCN_KIDS_PETS",id=33,},[10]={name="INDOOR",id=34,},[11]={name="SCN_SUNSET",id=55,},} */

	CR (gp_widget_new (GP_WIDGET_RADIO, _(menu->label), widget));
	return GP_OK;
}

static int
chdk_put_capmode(CONFIG_PUT_ARGS) {
	char *val;
	char lua[100];

	gp_widget_get_value (widget, &val);
	/* integer? */
	sprintf(lua,"set_capture_mode('%s')\n", val);
	return chdk_generic_script_run (params, lua, NULL, NULL, context);
}

struct submenu imgsettings[] = {
	{ N_("ISO"),		"iso",		chdk_get_iso,	chdk_put_iso},
	{ N_("Aperture"),	"aperture",	chdk_get_av,	chdk_put_av},
	{ N_("Shutterspeed"),	"shutterspeed",	chdk_get_tv,	chdk_put_tv},
	{ N_("Focus"),		"focus",	chdk_get_focus,	chdk_put_focus},
	{ N_("Zoom"),		"zoom",		chdk_get_zoom,	chdk_put_zoom},
	{ N_("Press"),		"press",	chdk_get_press,	chdk_put_press},
	{ N_("Release"),	"release",	chdk_get_release,chdk_put_release},
	{ N_("Click"),		"click",	chdk_get_click,	chdk_put_click},
	{ N_("Capture Mode"),	"capmode",	chdk_get_capmode,chdk_put_capmode},
	{ NULL,			NULL,		NULL, 		NULL},
};

/* We have way less options than regular PTP now, but try to keep the same structure */
static int
chdk_camera_get_config (Camera *camera, CameraWidget **window, GPContext *context)
{
	PTPParams	*params = &(camera->pl->params);
	CameraWidget	*menu, *child;
	int		i, ret;

	CR(camera_prepare_chdk_capture(camera, context));

        gp_widget_new (GP_WIDGET_WINDOW, _("Camera and Driver Configuration"), window);
	gp_widget_set_name (*window, "main");
	gp_widget_new (GP_WIDGET_SECTION, _("Image Settings"), &menu);
	gp_widget_set_name (menu, "imgsettings");
	gp_widget_append(*window, menu);

	for (i=0;imgsettings[i].name;i++) {
		ret = imgsettings[i].getfunc(params,&imgsettings[i],&child,context);
		if (ret != GP_OK) {
			GP_LOG_E("error getting %s menu", imgsettings[i].name);
			continue;
		}
		gp_widget_set_name (child, imgsettings[i].name);
		gp_widget_append (menu, child);
	}
	return GP_OK;
}

static int
chdk_camera_set_config (Camera *camera, CameraWidget *window, GPContext *context)
{
	PTPParams	*params = &(camera->pl->params);
	int		i, ret;

	for (i=0;imgsettings[i].name;i++) {
		CameraWidget *widget;

		ret = gp_widget_get_child_by_label (window, _(imgsettings[i].label), &widget);
		if (ret != GP_OK)
			continue;
		if (!gp_widget_changed (widget))
			continue;
		ret = imgsettings[i].putfunc(params,widget,context);
		if (ret != GP_OK) {
			GP_LOG_E("error putting %s menu", imgsettings[i].name);
			continue;
		}
	}
	return GP_OK;
}

static int
chdk_camera_exit (Camera *camera, GPContext *context) 
{
	camera_unprepare_chdk_capture(camera, context);
        return GP_OK;
}

static int
chdk_camera_capture (Camera *camera, CameraCaptureType type, CameraFilePath *path,
	GPContext *context)
{
	int		ret, retint;
	char		*table, *s;
	PTPParams	*params = &camera->pl->params;
	char		*lua;
	const char	*luascript =	PTP_CHDK_LUA_SERIALIZE_MSGS \
				PTP_CHDK_LUA_RLIB_SHOOT	\
				"return rlib_shoot({info=true});\n";

	ret =  camera_prepare_chdk_capture(camera, context);
	if (ret != GP_OK) return ret;

	lua = malloc(strlen(luascript)+1);
	sprintf(lua,luascript); /* This expands the %q inside the string too ... do not optimize away. */
	ret = chdk_generic_script_run (params, lua, &table, &retint, context);
	free (lua);
	GP_LOG_D("rlib_shoot returned table %s, retint %d\n", table, retint);
	s = strstr(table, "exp=");
	if (s) {
		int exp;
		if (!sscanf(s,"exp=%d\n", &exp)) {
			GP_LOG_E("%s did not parse for exp=NR?", s);
			ret = GP_ERROR;
		} else {
			sprintf(path->name,"IMG_%04d.JPG", exp);
		}
	} else {
		GP_LOG_E("no exp=nr found?\n");
		ret = GP_ERROR;
	}
	s = strstr(table, "dir=\"A");
	if (s) {
		char *y = strchr(s+6,'"');
		if (y) *y='\0';
		strcpy(path->folder, s+6);
	} else {
		ret = GP_ERROR;
	}
	free (table);
        return ret;
}

int
chdk_init(Camera *camera, GPContext *context) {
        camera->functions->about = chdk_camera_about;
        camera->functions->exit = chdk_camera_exit;
        camera->functions->capture = chdk_camera_capture;
        camera->functions->summary = chdk_camera_summary;
        camera->functions->get_config = chdk_camera_get_config;
        camera->functions->set_config = chdk_camera_set_config;
/*
        camera->functions->trigger_capture = camera_trigger_capture;
        camera->functions->capture_preview = camera_capture_preview;
        camera->functions->wait_for_event = camera_wait_for_event;
*/

	gp_filesystem_set_funcs ( camera->fs, &chdk_fsfuncs, camera);
	return GP_OK;
}
