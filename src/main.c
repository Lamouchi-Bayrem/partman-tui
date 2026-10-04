#define _GNU_SOURCE
#include <ncurses.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 256
#define MAX_LINE 2048
#define LOG_FILE "/var/log/partman-tui.log"
#define FSTAB "/etc/fstab"

typedef struct {
    char name[PATH_MAX], type[32], fstype[64], label[128], uuid[128];
    char size[32], mountpoint[PATH_MAX], model[128];
} Device;

static Device devices[MAX_DEVICES];
static int device_count = 0;
static int selected = 0;
static char status_msg[512] = "Ready";

static void log_msg(const char *level, const char *fmt, ...) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char ts[64]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tmv);
    fprintf(f, "%s [%s] ", ts, level);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int st;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -errno;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + WTERMSIG(st);
}

static void unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') {
        memmove(s, s+1, n-2); s[n-2] = 0;
    }
}

static void field(const char *line, const char *key, char *out, size_t outsz) {
    char pat[64]; snprintf(pat, sizeof pat, "%s=\"", key);
    const char *p = strstr(line, pat);
    if (!p) { out[0]=0; return; }
    p += strlen(pat); const char *q=p;
    while (*q && *q!='"') q++;
    size_t n=(size_t)(q-p); if(n>=outsz)n=outsz-1;
    memcpy(out,p,n); out[n]=0; unquote(out);
}

static int scan_devices(void) {
    device_count=0;
    FILE *p=popen("lsblk -P -p -o NAME,TYPE,FSTYPE,LABEL,UUID,SIZE,MOUNTPOINT,MODEL 2>/dev/null", "r");
    if(!p){ snprintf(status_msg,sizeof status_msg,"lsblk failed: %s",strerror(errno)); return -1; }
    char line[MAX_LINE];
    while(fgets(line,sizeof line,p) && device_count<MAX_DEVICES){
        Device *d=&devices[device_count]; memset(d,0,sizeof *d);
        field(line,"NAME",d->name,sizeof d->name); field(line,"TYPE",d->type,sizeof d->type);
        field(line,"FSTYPE",d->fstype,sizeof d->fstype); field(line,"LABEL",d->label,sizeof d->label);
        field(line,"UUID",d->uuid,sizeof d->uuid); field(line,"SIZE",d->size,sizeof d->size);
        field(line,"MOUNTPOINT",d->mountpoint,sizeof d->mountpoint); field(line,"MODEL",d->model,sizeof d->model);
        if(d->name[0]) device_count++;
    }
    int rc=pclose(p); if(selected>=device_count) selected=device_count?device_count-1:0;
    snprintf(status_msg,sizeof status_msg,"Scanned %d block devices",device_count);
    log_msg("INFO","scan complete: %d entries, rc=%d",device_count,rc);
    return rc;
}

static bool input_box(const char *title, char *buf, size_t n) {
    int h=7,w=COLS>80?80:COLS-4,y=(LINES-h)/2,x=(COLS-w)/2;
    WINDOW *win=newwin(h,w,y,x); box(win,0,0); mvwprintw(win,1,2,"%s",title);
    mvwprintw(win,3,2,"> "); wrefresh(win); echo(); curs_set(1);
    wgetnstr(win,buf,(int)n-1); noecho(); curs_set(0); delwin(win);
    return buf[0]!=0;
}

static bool confirm_phrase(const char *text, const char *phrase) {
    char b[128]={0}; char title[256]; snprintf(title,sizeof title,"%s Type %s",text,phrase);
    return input_box(title,b,sizeof b) && strcmp(b,phrase)==0;
}

static int ensure_dir(const char *path) {
    char tmp[PATH_MAX]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char *p=tmp+1;*p;p++) if(*p=='/'){*p=0;if(mkdir(tmp,0755)&&errno!=EEXIST)return -errno;*p='/';}
    if(mkdir(tmp,0755)&&errno!=EEXIST)return -errno; return 0;
}

static void do_mount(void) {
    if(!device_count) return; Device *d=&devices[selected];
    if(strcmp(d->type,"part")&&strcmp(d->type,"crypt")&&strcmp(d->type,"lvm")){
        snprintf(status_msg,sizeof status_msg,"Select a partition, crypt, or LVM filesystem"); return;
    }
    if(!d->fstype[0]){snprintf(status_msg,sizeof status_msg,"No filesystem detected on %s",d->name);return;}
    char target[PATH_MAX]={0};
    snprintf(target,sizeof target,"/mnt/%s",d->label[0]?d->label:strrchr(d->name,'/')+1);
    if(!input_box("Mount point (absolute path)",target,sizeof target))return;
    if(target[0]!='/'){snprintf(status_msg,sizeof status_msg,"Mount point must be absolute");return;}
    int rc=ensure_dir(target); if(rc){snprintf(status_msg,sizeof status_msg,"mkdir failed: %s",strerror(-rc));return;}
    char *args[]={"mount","--",d->name,target,NULL}; rc=run_argv(args);
    snprintf(status_msg,sizeof status_msg,rc?"Mount failed, rc=%d. Press D for diagnostics":"Mounted %s at %s",rc,target);
    log_msg(rc?"ERROR":"INFO","mount %s -> %s rc=%d",d->name,target,rc); scan_devices();
}

static void do_unmount(void) {
    if(!device_count)return; Device *d=&devices[selected];
    if(!d->mountpoint[0]){snprintf(status_msg,sizeof status_msg,"%s is not mounted",d->name);return;}
    if(!confirm_phrase("Unmount selected filesystem?","UNMOUNT"))return;
    char *args[]={"umount","--",d->mountpoint,NULL}; int rc=run_argv(args);
    snprintf(status_msg,sizeof status_msg,rc?"Unmount failed, rc=%d":"Unmounted %s",rc,d->mountpoint);
    log_msg(rc?"ERROR":"INFO","unmount %s rc=%d",d->mountpoint,rc); scan_devices();
}

static int append_fstab(Device *d, const char *target) {
    if(!d->uuid[0]||!d->fstype[0])return EINVAL;
    int fd=open(FSTAB,O_RDWR|O_CLOEXEC); if(fd<0)return errno;
    if(flock(fd,LOCK_EX)){int e=errno;close(fd);return e;}
    char backup[PATH_MAX]; time_t now=time(NULL); snprintf(backup,sizeof backup,"/etc/fstab.partman.%ld.bak",(long)now);
    char *cp[]={"cp","-a",FSTAB,backup,NULL}; int rc=run_argv(cp); if(rc){flock(fd,LOCK_UN);close(fd);return EIO;}
    FILE *f=fdopen(fd,"a"); if(!f){int e=errno;close(fd);return e;}
    fprintf(f,"\n# Added by partman-tui\nUUID=%s %s %s defaults,nofail 0 2\n",d->uuid,target,d->fstype);
    fflush(f); fsync(fd); flock(fd,LOCK_UN); fclose(f);
    char *reload[]={"systemctl","daemon-reload",NULL}; run_argv(reload);
    log_msg("INFO","fstab updated for %s at %s; backup=%s",d->name,target,backup); return 0;
}

static void do_save_fstab(void) {
    Device *d=&devices[selected];
    if(!d->uuid[0]||!d->fstype[0]){snprintf(status_msg,sizeof status_msg,"UUID/filesystem missing");return;}
    char target[PATH_MAX]={0}; snprintf(target,sizeof target,"/mnt/%s",d->label[0]?d->label:strrchr(d->name,'/')+1);
    if(!input_box("Persistent mount point",target,sizeof target))return;
    if(!confirm_phrase("Back up and update /etc/fstab?","SAVE"))return;
    int rc=ensure_dir(target); if(!rc)rc=append_fstab(d,target);
    snprintf(status_msg,sizeof status_msg,rc?"fstab update failed: %s":"Saved persistent mount for %s",rc?strerror(rc):"",d->name);
}

static void do_partition(void) {
    Device *d=&devices[selected];
    if(strcmp(d->type,"disk")){snprintf(status_msg,sizeof status_msg,"Select a whole disk, not a partition");return;}
    if(!confirm_phrase("DANGER: partition editing can destroy all data.","ERASE-RISK"))return;
    endwin(); char *args[]={"cfdisk",d->name,NULL}; int rc=run_argv(args); refresh();
    snprintf(status_msg,sizeof status_msg,"cfdisk returned rc=%d",rc); log_msg("WARN","cfdisk %s rc=%d",d->name,rc); scan_devices();
}

static void diagnostics(void) {
    endwin(); puts("\n=== partman-tui diagnostics ===");
    char *a[]={"lsblk","-f",NULL}; run_argv(a);
    puts("\n--- Recent kernel storage messages ---");
    char *b[]={"journalctl","-k","-n","40","--no-pager",NULL}; run_argv(b);
    puts("\n--- Recent application log ---"); fflush(stdout);
    char *c[]={"tail","-n","40",LOG_FILE,NULL}; run_argv(c);
    puts("\nPress Enter to return..."); getchar(); refresh();
}

static void draw(void) {
    erase(); mvprintw(0,2,"partman-tui | storage administration");
    mvprintw(1,2,"NAME                         TYPE   FS       SIZE     LABEL              MOUNTPOINT");
    int rows=LINES-5, start=selected>=rows?selected-rows+1:0;
    for(int i=start;i<device_count&&i<start+rows;i++){
        if(i==selected)attron(A_REVERSE);
        mvprintw(2+i-start,1,"%-28.28s %-6.6s %-8.8s %-8.8s %-18.18s %-30.30s",devices[i].name,devices[i].type,
            devices[i].fstype[0]?devices[i].fstype:"-",devices[i].size,devices[i].label[0]?devices[i].label:"-",
            devices[i].mountpoint[0]?devices[i].mountpoint:"-");
        if(i==selected)attroff(A_REVERSE);
    }
    mvprintw(LINES-3,1,"Up/Down select | R rescan | M mount | U unmount | S save fstab | P partition | D debug | Q quit");
    mvprintw(LINES-2,1,"Status: %-*.*s",COLS-10,COLS-10,status_msg); refresh();
}

int main(int argc,char **argv){
    if(geteuid()!=0){
        char **args=calloc((size_t)argc+2,sizeof(char*)); if(!args)return 1;
        args[0]="sudo"; for(int i=0;i<argc;i++)args[i+1]=argv[i]; execvp("sudo",args);
        fprintf(stderr,"Unable to acquire sudo: %s\n",strerror(errno)); return 77;
    }
    umask(022); log_msg("INFO","application start uid=%ld",(long)getuid());
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE); curs_set(0); scan_devices();
    int ch; while((ch=getch())!='q'&&ch!='Q'){
        switch(ch){case KEY_UP:if(selected>0)selected--;break;case KEY_DOWN:if(selected+1<device_count)selected++;break;
        case 'r':case 'R':scan_devices();break;case 'm':case 'M':do_mount();break;case 'u':case 'U':do_unmount();break;
        case 's':case 'S':do_save_fstab();break;case 'p':case 'P':do_partition();break;case 'd':case 'D':diagnostics();break;}
        draw();
    }
    endwin(); log_msg("INFO","application exit"); return 0;
}
