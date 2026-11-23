// SPDX-License-Identifier: GPL-2.0-or-later
// Actual driver archive error must not make optional learning fatal.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_pipelines.h"
#include "common/logging/log.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace {
std::mutex log_mutex;
std::vector<std::string> messages;
void Require(bool value,const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string Bytes(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}
bool Saw(std::string_view text) {
    std::lock_guard lock(log_mutex);
    for (const auto& message:messages) if (message.find(text)!=std::string::npos) return true;
    return false;
}
}
namespace Log {
void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,const char* format,
                      const fmt::format_args& args) {
    std::lock_guard lock(log_mutex);messages.push_back(fmt::vformat(format,args));
}
}
// Forward genuine archive operations, forcing only its output destination to
// an invalid filesystem path. The error result comes from Apple's serializer.
@interface FailingArchiveDestination : NSObject
@property(nonatomic, strong) id<MTLBinaryArchive> underlying;
@property(nonatomic, strong) NSError* failure;
@end
@implementation FailingArchiveDestination
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor
                                          error:(NSError**)error {
    return [self.underlying addRenderPipelineFunctionsWithDescriptor:descriptor error:error];
}
- (BOOL)serializeToURL:(NSURL*)url error:(NSError**)error {
    NSError* actual=nil;
    const BOOL saved=[self.underlying serializeToURL:[NSURL fileURLWithPath:@"/dev/null/cara-cache.metallib"] error:&actual];
    self.failure=actual;if (error) *error=actual;return saved;
}
@end
namespace Cara {
struct PicaMetalPipelineCacheTestAccess {
    static void Configure(PicaMetalPipelines& p,const std::string& path,unsigned mode,
                          id<MTLBinaryArchive> archive,MTLRenderPipelineDescriptor* descriptor) {
        p.warmup_mode=mode;p.warmup_path=path;p.warmup_archive=archive;
        PicaMetalPipelines::WarmupRecipe recipe;
        recipe.vertex="authored vertex recipe";recipe.fragment="authored fragment recipe";
        p.warmup_recipes={recipe};p.warmup_new_recipes=1;
        p.warmup_descriptors=descriptor ? std::vector<MTLRenderPipelineDescriptor*>{descriptor} :
                                        std::vector<MTLRenderPipelineDescriptor*>{};
    }
    static bool Save(PicaMetalPipelines& p) { return p.SaveWarmup(); }
    static void RecordingFailed(PicaMetalPipelines& p) { p.warmup_recording_failed=true; }
    static void ReplayMiss(PicaMetalPipelines& p) { p.warmup_draw_misses=1; }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Expected fixture output directory");
    const std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
    const auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"No Metal device");
    NSError* error=nil;
    const auto library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint i [[vertex_id]]) { return float4(0); }\nfragment float4 f() { return float4(1); }" options:nil error:&error];
    Require(library!=nil,"Authored shader failed");
    auto* valid=[MTLRenderPipelineDescriptor new];
    valid.vertexFunction=[library newFunctionWithName:@"v"];
    valid.fragmentFunction=[library newFunctionWithName:@"f"];
    valid.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    const auto base=(dir/"preserve").string();
    const auto archive=[device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
    Require(archive && [archive addRenderPipelineFunctionsWithDescriptor:valid error:&error],"Valid archive add failed");
    Require([archive serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:(base+".metallib").c_str()]] error:&error],"Valid archive serialize failed");
    const auto original=Bytes(base+".metallib");Require(!original.empty(),"Empty original archive");
    auto* failing=[FailingArchiveDestination new];failing.underlying=archive;
    Cara::PicaMetalPipelines learned(device);
    Cara::PicaMetalPipelineCacheTestAccess::Configure(learned,base,3,(id<MTLBinaryArchive>)failing,valid);
    Require(!Cara::PicaMetalPipelineCacheTestAccess::Save(learned),"Driver serializer error must report persistence failure");
    Require(failing.failure!=nil,"Expected actual Apple serializer NSError");
    std::cout<<"Driver serializer NSError: "<<failing.failure.localizedDescription.UTF8String<<'\n';
    Require(Bytes(base+".metallib")==original,"Failed archive replaced old valid bytes");
    const auto recipe=Bytes(base+".recipes");
    Require(recipe.starts_with("CaraMetalRecipes1") && recipe.find("authored vertex recipe")!=std::string::npos,"Recipes were lost on archive failure");
    Require(learned.FinishWarmup(),"Learn archive failure became fatal");
    Require(Saw("persistence incomplete"),"Learn archive failure warning missing");
    Cara::PicaMetalPipelines unwritable(device);
    Cara::PicaMetalPipelineCacheTestAccess::Configure(unwritable,(dir/"absent"/"cache").string(),3,nil,nil);
    Require(!Cara::PicaMetalPipelineCacheTestAccess::Save(unwritable),"Missing parent path must fail persistence");
    Require(unwritable.FinishWarmup(),"Learn file failure became fatal");
    Cara::PicaMetalPipelines record(device);
    Cara::PicaMetalPipelineCacheTestAccess::Configure(record,(dir/"absent"/"strict").string(),1,nil,nil);
    Require(!record.FinishWarmup(),"Record write failure accepted");
    Cara::PicaMetalPipelineCacheTestAccess::RecordingFailed(record);
    Require(!record.FinishWarmup(),"Record validation failure accepted");
    Cara::PicaMetalPipelines replay(device);
    Cara::PicaMetalPipelineCacheTestAccess::Configure(replay,base,2,nil,nil);
    Require(replay.FinishWarmup(),"Empty replay should pass");
    Cara::PicaMetalPipelineCacheTestAccess::ReplayMiss(replay);
    Require(!replay.FinishWarmup(),"Replay missing draw accepted");
    for (const auto& entry:std::filesystem::directory_iterator(dir))
        Require(entry.path().extension()==".recipes" || entry.path().extension()==".metallib","Temporary cache file leaked");
    std::cout<<"PASS real driver archive failure, preserved archive bytes, recipe publication, nonfatal learning, strict record/replay\n";
    return 0;
} catch(const std::exception& failure) { std::cerr<<failure.what()<<'\n';return 1; } } }
