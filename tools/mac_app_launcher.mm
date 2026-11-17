// Copyright 2026 Cara contributors. GPL-2.0-or-later.
#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static void Require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
static NSDictionary* JSON(NSString* path) {
    NSData* data=[NSData dataWithContentsOfFile:path];
    id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nullptr]:nil;
    Require([value isKindOfClass:NSDictionary.class],"The application metadata is missing or invalid");
    return value;
}
static NSString* Digest(NSString* path) {
    NSFileHandle* f=[NSFileHandle fileHandleForReadingAtPath:path];
    Require(f!=nil,"The gameplay executable is missing");
    CC_SHA256_CTX state;CC_SHA256_Init(&state);
    for (;;) {NSData* data=[f readDataOfLength:1024*1024];if(!data.length)break;CC_SHA256_Update(&state,data.bytes,(CC_LONG)data.length);}
    [f closeFile];unsigned char bytes[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(bytes,&state);
    NSMutableString* result=[NSMutableString string];for(auto byte:bytes)[result appendFormat:@"%02x",byte];return result;
}
int main(int argc, char** argv) {
    const bool verify_package=argc==2 && std::strcmp(argv[1],"--verify-package")==0;
    @autoreleasepool {try {
        NSString* contents=[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents"];
        NSString* resources=[contents stringByAppendingPathComponent:@"Resources"];
        NSDictionary* manifest=JSON([resources stringByAppendingPathComponent:@"build.json"]);
        NSString* binary=[contents stringByAppendingPathComponent:@"MacOS/cara-core"];
        Require([Digest(binary) isEqual:manifest[@"binary_sha256"]],"The gameplay executable differs from the verified build");
        NSString* user=[manifest[@"user_dir"] stringByExpandingTildeInPath];
        Require([user isAbsolutePath],"Invalid game data directory");
        if(!verify_package){
        struct stat st{};if(lstat(user.fileSystemRepresentation,&st)==0)Require(!S_ISLNK(st.st_mode),"The game data directory must not be a symbolic link");
        NSError* error=nil;
        Require([[NSFileManager defaultManager]createDirectoryAtPath:user withIntermediateDirectories:YES attributes:nil error:&error],"Could not create the game data directory");
        int lock=open([[user stringByAppendingPathComponent:@".gameplay.lock"] fileSystemRepresentation],O_RDWR|O_CREAT|O_NOFOLLOW,0600);
        Require(lock>=0 && flock(lock,LOCK_EX|LOCK_NB)==0,"Cara is already using this game data directory");
        // Keep this descriptor through both execs, including File > Open's normal restart.
        fcntl(lock,F_SETFD,0);
        }
        std::vector<char*> args{const_cast<char*>(binary.fileSystemRepresentation)};
        if(verify_package)args.push_back(const_cast<char*>("--help"));
        else for(int i=1;i<argc;++i)if(std::strncmp(argv[i],"-psn_",5)!=0)args.push_back(argv[i]);
        Require(args.size()<=2,"Choose a game with Open Game in Cara");
        if(!verify_package && args.size()==2)Require([[NSFileManager defaultManager]fileExistsAtPath:[NSString stringWithUTF8String:args[1]]],"Game file not found");
        args.push_back(nullptr);
        for(NSString* name in NSProcessInfo.processInfo.environment)
            if([name hasPrefix:@"CARA_"] || ([name hasPrefix:@"DYLD_"] && !(verify_package && [name isEqual:@"DYLD_PRINT_LIBRARIES"])) || [name isEqual:@"SDL_DYNAMIC_API"] || [name isEqual:@"SDL3_DYNAMIC_API"])unsetenv(name.UTF8String);
        NSDictionary* runtime=JSON([resources stringByAppendingPathComponent:@"runtime.json"]);
        for(NSString* name in runtime){Require([runtime[name] isKindOfClass:NSString.class],"Invalid runtime settings");setenv(name.UTF8String,[runtime[name] UTF8String],1);}
        if(!verify_package)setenv("CARA_USER_DIR",user.fileSystemRepresentation,1);
        if(!verify_package){
        int log=open([[user stringByAppendingPathComponent:@"last-launch.log"] fileSystemRepresentation],O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        Require(log>=0,"Could not open the game log");dup2(log,STDOUT_FILENO);dup2(log,STDERR_FILENO);close(log);
        }
        execv(binary.fileSystemRepresentation,args.data());
        throw std::runtime_error("Could not start Cara");
    }catch(const std::exception& e){
        if(verify_package){std::fprintf(stderr,"Cara package verification: %s\n",e.what());return 1;}
        [NSApplication sharedApplication];NSAlert* alert=[[NSAlert alloc]init];alert.messageText=@"Cara";alert.informativeText=[NSString stringWithUTF8String:e.what()];[alert runModal];return 1;
    }}
}
