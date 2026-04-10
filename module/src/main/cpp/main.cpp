#include <cstring>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cinttypes>
#include <string>
#include <sstream>
#include "hack.h"
#include "zygisk.hpp"
#include "game.h"
#include "log.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        loadConfig();
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        auto app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        preSpecialize(package_name, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (enable_hack) {
            auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);

            LOGI("Game uid: %d", args->uid);
            LOGI("Game gid: %d", args->gid);
            LOGI("Process pid: %d", gettid());
            LOGI("Process name: %s", package_name);

            std::thread hack_thread(hack_prepare, game_data_dir, data, gettid(), length);
            hack_thread.detach();
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *game_data_dir;
    void *data;
    size_t length;
    std::string target_package;

    void loadConfig() {
        int dirfd = api->getModuleDir();
        int fd = openat(dirfd, "config.txt", O_RDONLY);
        if (fd == -1) {
            LOGW("config.txt not found, using default: %s", GamePackageName);
            target_package = GamePackageName;
        } else {
            char buf[512] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            bool found = false;
            if (n > 0) {
                std::istringstream ss(std::string(buf, n));
                std::string line;
                while (std::getline(ss, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty() && line[0] != '#') {
                        target_package = line;
                        LOGI("Target package from config: %s", target_package.c_str());
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                LOGW("No valid package in config.txt, using default: %s", GamePackageName);
                target_package = GamePackageName;
            }
        }
        updateModuleProp();
    }

    void updateModuleProp() {
        int dirfd = api->getModuleDir();
        int fd = openat(dirfd, "module.prop", O_RDONLY);
        if (fd == -1) {
            LOGW("module.prop not found, skip update");
            return;
        }
        char buf[1024] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return;

        std::string content(buf, n);
        std::istringstream ss(content);
        std::string result;
        std::string line;
        bool updated = false;
        while (std::getline(ss, line)) {
            if (line.rfind("description=", 0) == 0) {
                // 取 = 后面的原始描述（去掉上次追加的包名部分）
                std::string base = line.substr(12);
                auto sep = base.find(" | target: ");
                if (sep != std::string::npos) base = base.substr(0, sep);
                result += "description=" + base + " | target: " + target_package + "\n";
                updated = true;
            } else {
                result += line + "\n";
            }
        }
        if (!updated) return;

        fd = openat(dirfd, "module.prop", O_WRONLY | O_TRUNC);
        if (fd == -1) {
            LOGW("Cannot write module.prop");
            return;
        }
        write(fd, result.c_str(), result.size());
        close(fd);
        LOGI("module.prop updated with target: %s", target_package.c_str());
    }

    void preSpecialize(const char *package_name, const char *app_data_dir) {
        if (strcmp(package_name, target_package.c_str()) == 0) {
            LOGI("detect game: %s", package_name);

            enable_hack = true;
            game_data_dir = new char[strlen(app_data_dir) + 1];
            strcpy(game_data_dir, app_data_dir);

#if defined(__i386__)
            auto path = "zygisk/armeabi-v7a.so";
#endif
#if defined(__x86_64__)
            auto path = "zygisk/arm64-v8a.so";
#endif

#if defined(__i386__) || defined(__x86_64__)
            int dirfd = api->getModuleDir();
            int fd = openat(dirfd, path, O_RDONLY);
            if (fd != -1) {
                struct stat sb{};
                fstat(fd, &sb);
                length = sb.st_size;
                data = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
            } else {
                LOGW("Unable to open arm file");
            }
#endif
        } else {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)