#include "core.hpp"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <limits.h>

using namespace linuxguard;
namespace {
struct State {
    GtkWidget* window=nullptr;
    GtkWidget* device=nullptr;
    GtkWidget* title=nullptr;
    GtkWidget* detail=nullptr;
    GtkWidget* progress=nullptr;
    GtkWidget* actionButtons[4]{};
    std::vector<Disk> disks;
    std::string helper;
    bool working=false;
    bool finished=false;
    bool success=false;
    int percent=0;
    unsigned closeTimer=0;
} ui;

struct Event {
    std::string text;
    int done=0;
    int total=0;
    bool finished=false;
    bool success=false;
};

gboolean CloseLater(gpointer) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
void EnableActions(bool enabled) {
    gtk_widget_set_sensitive(ui.device, enabled);
    for(auto* button:ui.actionButtons)
        if(button)gtk_widget_set_sensitive(button,enabled);
}
gboolean ProcessEvent(gpointer data) {
    std::unique_ptr<Event> event(static_cast<Event*>(data));
    if(event->finished) {
        ui.working=false;
        ui.finished=true;
        ui.success=event->success;
        gtk_label_set_text(GTK_LABEL(ui.title),event->success?"操作已完成":"操作未完成");
        gtk_label_set_text(GTK_LABEL(ui.detail),event->text.c_str());
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress),event->success?1.0:static_cast<double>(ui.percent)/100);
        if(!event->success) EnableActions(true);
        else ui.closeTimer=g_timeout_add(4200,CloseLater,nullptr);
    } else {
        gtk_label_set_text(GTK_LABEL(ui.title),"正在切换写保护");
        gtk_label_set_text(GTK_LABEL(ui.detail),event->text.c_str());
        if(event->total>0) {
            ui.percent=std::clamp(event->done*100/event->total,0,100);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress),ui.percent/100.0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress),
                                      (std::to_string(ui.percent)+"%").c_str());
        }
    }
    return G_SOURCE_REMOVE;
}
void Post(std::unique_ptr<Event> event) {
    g_idle_add(ProcessEvent,event.release());
}
std::string ExecutableDirectory() {
    char exe[PATH_MAX]{};
    ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(n<=0) return {};
    std::string full(exe,static_cast<size_t>(n));
    size_t slash=full.find_last_of('/');
    return slash==std::string::npos?std::string{}:full.substr(0,slash+1);
}
std::string HelperPath() {
    const auto folder=ExecutableDirectory();
    if(folder.empty())return "USBGuardLinuxCLI";
    return folder+"USBGuardLinuxCLI";
}
std::string CommandExample(const std::string& disk,Action action) {
    const char* flag=action==Action::Protect?"--protect":
                     action==Action::Unprotect?"--unprotect":
                     action==Action::Status?"--status":"--toggle";
    return "sudo "+EscapeShellArgument(ui.helper)+" --device "+
           EscapeShellArgument(disk)+" "+flag;
}
void Work(const std::string disk,Action action) {
    std::string flag=action==Action::Protect?"--protect":
                     action==Action::Unprotect?"--unprotect":
                     action==Action::Status?"--status":"--toggle";
    const bool elevated = geteuid() == 0;
    int fds[2];
    if(pipe(fds)!=0) {
        Post(std::make_unique<Event>(Event{"无法创建子进程管道",0,0,true,false}));
        return;
    }
    pid_t pid=fork();
    if(pid<0) {
        close(fds[0]);close(fds[1]);
        Post(std::make_unique<Event>(Event{"无法创建权限请求进程",0,0,true,false}));
        return;
    }
    if(pid==0) {
        close(fds[0]);
        dup2(fds[1],STDOUT_FILENO);
        dup2(fds[1],STDERR_FILENO);
        close(fds[1]);
        if(elevated || action==Action::Status) {
            execl(ui.helper.c_str(),ui.helper.c_str(),"--device",disk.c_str(),
                  flag.c_str(),static_cast<char*>(nullptr));
        }else {
            execlp("pkexec","pkexec",ui.helper.c_str(),"--device",disk.c_str(),
                   flag.c_str(),static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    close(fds[1]);
    FILE* input=fdopen(fds[0],"r");
    char* line=nullptr;size_t capacity=0;
    std::string finalMessage;
    while(input && getline(&line,&capacity,input)!=-1) {
        std::string message(line);
        if(!message.empty()&&message.back()=='\n')message.pop_back();
        if(message.rfind("PROGRESS ",0)==0) {
            int done=0,total=0;
            const auto slash=message.find('/');
            const auto space=message.find(' ',slash+1);
            if(slash!=std::string::npos&&space!=std::string::npos) {
                try {
                    done=std::stoi(message.substr(9,slash-9));
                    total=std::stoi(message.substr(slash+1,space-slash-1));
                    Post(std::make_unique<Event>(Event{message.substr(space+1),done,total,false,false}));
                }catch(...){}
            }
        }else if(message.rfind("RESULT ",0)==0)finalMessage=message.substr(7);
        else if(message.rfind("ERROR ",0)==0)finalMessage=message.substr(6);
    }
    free(line);
    if(input)fclose(input);else close(fds[0]);
    int status=0;
    if(waitpid(pid,&status,0)<0)status=-1;
    const bool success=WIFEXITED(status)&&WEXITSTATUS(status)==0;
    if(finalMessage.empty())finalMessage=success?"操作完成":"权限请求或操作失败";
    if(!success&&!elevated&&action!=Action::Status)
        finalMessage+="\n如果当前桌面缺少 polkit 授权代理，可在终端执行：\n"+
                      CommandExample(disk,action);
    Post(std::make_unique<Event>(Event{finalMessage,0,0,true,success}));
}
void OnAction(GtkButton*,gpointer data) {
    if(ui.working)return;
    const gchar* active=gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.device));
    if(!active)return;
    const int number=GPOINTER_TO_INT(data);
    const Action action=number==0?Action::Toggle:
                        number==1?Action::Protect:
                        number==2?Action::Unprotect:Action::Status;
    ui.working=true;ui.finished=false;ui.percent=0;
    EnableActions(false);
    gtk_label_set_text(GTK_LABEL(ui.title),action==Action::Status?
                       "正在读取磁盘状态":"正在请求管理员权限");
    gtk_label_set_text(GTK_LABEL(ui.detail),action==Action::Status?
                       "正在检查当前挂载点和块设备":"请在系统授权窗口中允许 USBGuard 修改 USB 设备状态。");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress),0);
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ui.progress));
    if(ui.closeTimer){g_source_remove(ui.closeTimer);ui.closeTimer=0;}
    std::thread(Work,std::string(active),action).detach();
}
void OnRefresh(GtkButton*,gpointer) {
    ui.disks=Discover();
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.device));
    const auto preferred=SourceDisk();
    int selected=-1;
    for(size_t i=0;i<ui.disks.size();++i) {
        const auto& disk=ui.disks[i];
        std::string label=disk.device+"  "+disk.model+"   ";
        for(const auto& volume:disk.volumes)label+=volume.mountpoint+" ";
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.device),disk.device.c_str(),label.c_str());
        if(disk.device==preferred)selected=static_cast<int>(i);
    }
    if(selected<0&&!ui.disks.empty())selected=0;
    if(selected>=0)gtk_combo_box_set_active(GTK_COMBO_BOX(ui.device),selected);
    gtk_label_set_text(GTK_LABEL(ui.detail),ui.disks.empty()?
        "没有检测到已挂载的 USB 磁盘。请插入设备，然后刷新列表。":
        "操作将影响所选 USB 磁盘当前已挂载的文件系统。");
    gtk_widget_set_sensitive(ui.device,!ui.disks.empty());
    for(auto* btn:ui.actionButtons)if(btn)gtk_widget_set_sensitive(btn,!ui.disks.empty());
}
gboolean OnDelete(GtkWidget*,GdkEvent*,gpointer) {
    // The privileged worker must be allowed to finish before the GUI exits.
    if(ui.working) {
        gtk_label_set_text(GTK_LABEL(ui.detail),
                           "设备操作正在执行，请等待卷状态检查结束。");
        return TRUE;
    }
    return FALSE;
}
void OnDestroy(GtkWidget*,gpointer){gtk_main_quit();}
void LoadStyles() {
    const char* css=R"css(
        window {background:#f7fafc}
        #top {background:#133451;padding:20px}
        #brand {color:#ffffff;font-size:21px;font-weight:800}
        #subbrand {color:#b5d4e5;font-size:12px}
        #body {padding:22px}
        #heading {font-size:17px;font-weight:700;color:#163551}
        #info {font-size:12px;color:#536c80}
        button {border-radius:9px;padding:9px 15px}
        button.suggested-action {background:#1398b7;color:white}
        progressbar trough {min-height:10px;border-radius:6px;background:#dce8ef}
        progressbar progress {min-height:10px;border-radius:6px;background:#16a0bd}
    )css";
    GtkCssProvider* provider=gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,css,-1,nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}
GtkWidget* StyledLabel(const char* content,const char* name) {
    GtkWidget* label=gtk_label_new(content);
    gtk_widget_set_name(label,name);
    gtk_label_set_xalign(GTK_LABEL(label),0);
    return label;
}
void BuildUI() {
    ui.window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window),"USBGuard Linux");
    gtk_window_set_default_size(GTK_WINDOW(ui.window),490,300);
    gtk_window_set_resizable(GTK_WINDOW(ui.window),FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(ui.window),0);
    gtk_window_set_position(GTK_WINDOW(ui.window),GTK_WIN_POS_CENTER);
    g_signal_connect(ui.window,"delete-event",G_CALLBACK(OnDelete),nullptr);
    g_signal_connect(ui.window,"destroy",G_CALLBACK(OnDestroy),nullptr);
    GtkWidget* outer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    gtk_container_add(GTK_CONTAINER(ui.window),outer);
    GtkWidget* top=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
    gtk_widget_set_name(top,"top");
    gtk_box_pack_start(GTK_BOX(outer),top,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(top),StyledLabel("USB GUARD","brand"),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(top),StyledLabel("Linux · U 盘保护","subbrand"),FALSE,FALSE,0);
    GtkWidget* body=gtk_box_new(GTK_ORIENTATION_VERTICAL,15);
    gtk_widget_set_name(body,"body");
    gtk_box_pack_start(GTK_BOX(outer),body,TRUE,TRUE,0);
    ui.title=StyledLabel("选择 USB 磁盘","heading");
    gtk_box_pack_start(GTK_BOX(body),ui.title,FALSE,FALSE,0);
    GtkWidget* selector=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(body),selector,FALSE,FALSE,0);
    ui.device=gtk_combo_box_text_new();
    gtk_widget_set_hexpand(ui.device,TRUE);
    gtk_box_pack_start(GTK_BOX(selector),ui.device,TRUE,TRUE,0);
    GtkWidget* refresh=gtk_button_new_with_label("刷新");
    gtk_box_pack_start(GTK_BOX(selector),refresh,FALSE,FALSE,0);
    g_signal_connect(refresh,"clicked",G_CALLBACK(OnRefresh),nullptr);
    ui.detail=StyledLabel("正在检测可访问的 USB 分区…","info");
    gtk_label_set_line_wrap(GTK_LABEL(ui.detail),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.detail),59);
    gtk_box_pack_start(GTK_BOX(body),ui.detail,FALSE,FALSE,0);
    ui.progress=gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.progress),TRUE);
    gtk_box_pack_start(GTK_BOX(body),ui.progress,FALSE,FALSE,0);
    GtkWidget* actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(body),actions,FALSE,FALSE,0);
    const char* labels[]={"切换保护","启用只读","解除只读","查看状态"};
    for(int i=0;i<4;++i){
        ui.actionButtons[i]=gtk_button_new_with_label(labels[i]);
        gtk_box_pack_start(GTK_BOX(actions),ui.actionButtons[i],TRUE,TRUE,0);
        if(i==0)gtk_style_context_add_class(gtk_widget_get_style_context(ui.actionButtons[i]),
                                           "suggested-action");
        g_signal_connect(ui.actionButtons[i],"clicked",G_CALLBACK(OnAction),GINT_TO_POINTER(i));
    }
    gtk_widget_show_all(ui.window);
    OnRefresh(nullptr,nullptr);
}
}
int main(int argc,char** argv) {
    const bool uiTest = argc == 2 && std::string(argv[1]) == "--ui-self-test";
    // GTK selects native Wayland or X11 dynamically based on the active session.
    if(!gtk_init_check(&argc,&argv)) {
        fprintf(stderr,"无法连接图形显示服务器。请使用 USBGuardLinuxCLI --list 或 --status。\n");
        return 5;
    }
    ui.helper=HelperPath();
    LoadStyles();
    BuildUI();
    if(uiTest) {
        Post(std::make_unique<Event>(Event{"窗口显示、进度更新和自动退出验证",1,2,false,false}));
        g_timeout_add(800,CloseLater,nullptr);
    }
    gtk_main();
    return 0;
}
