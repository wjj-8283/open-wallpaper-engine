#include "Particle/ParticleEmitter.h"
#include "Interface/IParticleRawGener.h"
#include "Particle/ParticleModify.h"
#include "Particle/ParticleSystem.h"
#include "Scene/Scene.h"
#include "WPParticleParser.hpp"
#include "wpscene/WPParticleObject.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>

namespace wallpaper
{
namespace
{

class CapturingParticleRawGener final : public IParticleRawGener {
public:
    void GenGLData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                   SceneMesh&,
                   ParticleRawGenSpecOp&) override {
        particles.clear();
        for (const auto& instance : instances) {
            if (instance) {
                const auto instanceParticles = instance->Particles();
                particles.insert(particles.end(), instanceParticles.begin(), instanceParticles.end());
            }
        }
    }

    std::vector<Particle> particles;
};

Particle FirstSpawnedParticle(ParticleEmittOp&                      emitter,
                              std::span<const ParticleControlpoint> controlpoints) {
    std::vector<Particle>       particles;
    std::vector<ParticleInitOp> initializers;

    emitter(particles, initializers, 8, 1.0, controlpoints);

    EXPECT_FALSE(particles.empty());
    return particles.front();
}

TEST(ParticleMouseControlpoint, EmitterControlpointOffsetsBoxSpawnOrigin) {
    ParticleBoxEmitterArgs args {};
    args.directions    = { 0.0f, 0.0f, 0.0f };
    args.minDistance   = { 0.0f, 0.0f, 0.0f };
    args.maxDistance   = { 0.0f, 0.0f, 0.0f };
    args.emitSpeed     = 1.0f;
    args.orgin         = { 10.0f, 20.0f, 30.0f };
    args.instantaneous = 1;
    args.controlpoint  = 2;

    std::array<ParticleControlpoint, 8> controlpoints {};
    controlpoints[2].offset = Eigen::Vector3d(5.0, 6.0, 7.0);

    auto           emitter  = ParticleBoxEmitterArgs::MakeEmittOp(args);
    const Particle particle = FirstSpawnedParticle(emitter, controlpoints);

    EXPECT_FLOAT_EQ(particle.position.x(), 15.0f);
    EXPECT_FLOAT_EQ(particle.position.y(), 26.0f);
    EXPECT_FLOAT_EQ(particle.position.z(), 37.0f);
}

TEST(ParticleMouseControlpoint, EmitterParsesAudioResponseParameters) {
    wpscene::Emitter emitter;

    ASSERT_TRUE(emitter.FromJson(nlohmann::json {
        { "name", "boxrandom" },
        { "audioprocessingmode", 2 },
        { "controlpoint", 3 },
        { "rate", 4.0f },
    }));

    EXPECT_EQ(emitter.audioprocessingmode, 2u);
    EXPECT_EQ(emitter.controlpoint, 3);
    EXPECT_FLOAT_EQ(emitter.rate, 4.0f);
}

TEST(ParticleMouseControlpoint, ControlpointAttractReadsThresholdAndOrigin) {
    nlohmann::json json {
        { "name", "controlpointattract" },
        { "controlpoint", 1 },
        { "scale", 10.0f },
        { "threshold", 4.0f },
        { "origin", { 2.0f, 0.0f, 0.0f } },
    };

    WPParticleParser parser;
    auto             op = parser.genParticleOperatorOp(json, wpscene::ParticleInstanceoverride {});

    std::array<ParticleControlpoint, 8> controlpoints {};
    controlpoints[1].offset = Eigen::Vector3d(3.0, 0.0, 0.0);

    std::array<Particle, 1> particles {};
    ParticleModify::MoveTo(particles[0], 0.0, 0.0, 0.0);

    ParticleInfo info {
        .particles     = particles,
        .controlpoints = controlpoints,
        .time          = 0.0,
        .time_pass     = 1.0,
    };
    op(info);

    EXPECT_FLOAT_EQ(particles[0].velocity.x(), 0.0f);

    json["threshold"] = 6.0f;
    op                = parser.genParticleOperatorOp(json, wpscene::ParticleInstanceoverride {});
    particles[0].velocity = Eigen::Vector3f::Zero();
    op(info);

    EXPECT_FLOAT_EQ(particles[0].velocity.x(), 10.0f);

    json["threadhold"] = 4.0f;
    op                 = parser.genParticleOperatorOp(json, wpscene::ParticleInstanceoverride {});
    particles[0].velocity = Eigen::Vector3f::Zero();
    op(info);

    EXPECT_FLOAT_EQ(particles[0].velocity.x(), 10.0f);
}

TEST(ParticleMouseControlpoint, ControlpointAttractNormalizesNegativeIndexSafely) {
    nlohmann::json json {
        { "name", "controlpointattract" },
        { "controlpoint", -1 },
        { "scale", 10.0f },
        { "threshold", 6.0f },
    };

    WPParticleParser parser;
    auto             op = parser.genParticleOperatorOp(json, wpscene::ParticleInstanceoverride {});

    std::array<ParticleControlpoint, 8> controlpoints {};
    controlpoints[0].offset = Eigen::Vector3d(5.0, 0.0, 0.0);

    std::array<Particle, 1> particles {};
    ParticleModify::MoveTo(particles[0], 0.0, 0.0, 0.0);

    ParticleInfo info {
        .particles     = particles,
        .controlpoints = controlpoints,
        .time          = 0.0,
        .time_pass     = 1.0,
    };
    op(info);

    EXPECT_FLOAT_EQ(particles[0].velocity.x(), 10.0f);
}

TEST(ParticleMouseControlpoint, ControlpointAttractSkipsZeroDistance) {
    nlohmann::json json {
        { "name", "controlpointattract" },
        { "controlpoint", 1 },
        { "scale", 10.0f },
        { "threshold", 6.0f },
        { "origin", { 2.0f, 0.0f, 0.0f } },
    };

    WPParticleParser parser;
    auto             op = parser.genParticleOperatorOp(json, wpscene::ParticleInstanceoverride {});

    std::array<ParticleControlpoint, 8> controlpoints {};
    controlpoints[1].offset = Eigen::Vector3d(3.0, 0.0, 0.0);

    std::array<Particle, 1> particles {};
    ParticleModify::MoveTo(particles[0], 5.0, 0.0, 0.0);

    ParticleInfo info {
        .particles     = particles,
        .controlpoints = controlpoints,
        .time          = 0.0,
        .time_pass     = 1.0,
    };
    op(info);

    EXPECT_TRUE(std::isfinite(particles[0].velocity.x()));
    EXPECT_TRUE(std::isfinite(particles[0].velocity.y()));
    EXPECT_TRUE(std::isfinite(particles[0].velocity.z()));
    EXPECT_FLOAT_EQ(particles[0].velocity.x(), 0.0f);
}

TEST(ParticleMouseControlpoint, MouseControlpointUpdatesEmitterOrigin) {
    Scene scene;
    scene.ortho[0]        = 200;
    scene.ortho[1]        = 100;
    scene.frameTime       = 1.0;
    scene.pointerPosition = { 0.75f, 0.25f };

    auto              mesh = std::make_shared<SceneMesh>(true);
    ParticleSystem    system(scene);
    ParticleSubSystem subsystem(system,
                                mesh,
                                8,
                                1.0,
                                1,
                                1.0,
                                ParticleSubSystem::SpawnType::STATIC,
                                [](const Particle&, const ParticleRawGenSpec&) {
                                });
    auto              controlpoints = subsystem.Controlpoints();
    controlpoints[0].link_mouse     = true;
    controlpoints[0].base_offset    = Eigen::Vector3d(1.0, 2.0, 3.0);
    controlpoints[0].offset         = controlpoints[0].base_offset;

    subsystem.UpdateMouseControlpoints();

    EXPECT_DOUBLE_EQ(controlpoints[0].offset.x(), 151.0);
    EXPECT_DOUBLE_EQ(controlpoints[0].offset.y(), 77.0);
    EXPECT_DOUBLE_EQ(controlpoints[0].offset.z(), 3.0);
}

TEST(ParticleMouseControlpoint, MovementIntegratesAfterControlpointAttractUpdatesVelocity) {
    Scene scene;
    scene.frameTime = 1.0;

    auto* rawGener = new CapturingParticleRawGener();
    auto  mesh     = std::make_shared<SceneMesh>(true);

    ParticleSystem system(scene);
    system.gener.reset(rawGener);

    ParticleSubSystem subsystem(system,
                                mesh,
                                8,
                                1.0,
                                1,
                                1.0,
                                ParticleSubSystem::SpawnType::STATIC,
                                [](const Particle&, const ParticleRawGenSpec&) {
                                });

    subsystem.Controlpoints()[0].offset = Eigen::Vector3d(10.0, 0.0, 0.0);

    subsystem.AddEmitter([](std::vector<Particle>& particles,
                            std::vector<ParticleInitOp>&,
                            uint32_t,
                            double,
                            std::span<const ParticleControlpoint>) {
        if (! particles.empty()) return;

        Particle particle;
        ParticleModify::MoveTo(particle, 0.0, 0.0, 0.0);
        ParticleModify::InitVelocity(particle, 1.0, 0.0, 0.0);
        ParticleModify::InitLifetime(particle, 2.0f);
        particles.emplace_back(particle);
    });

    WPParticleParser parser;
    subsystem.AddOperator(parser.genParticleOperatorOp(nlohmann::json {
                            { "name", "movement" },
                            { "gravity", { 0.0f, 0.0f, 0.0f } },
                            { "drag", 0.0f },
                        },
                                                       wpscene::ParticleInstanceoverride {}));
    subsystem.AddOperator(parser.genParticleOperatorOp(nlohmann::json {
                            { "name", "controlpointattract" },
                            { "controlpoint", 0 },
                            { "scale", 2.0f },
                            { "threshold", 20.0f },
                        },
                                                       wpscene::ParticleInstanceoverride {}));

    subsystem.Emitt();

    ASSERT_EQ(rawGener->particles.size(), 1u);
    EXPECT_FLOAT_EQ(rawGener->particles[0].velocity.x(), 3.0f);
    EXPECT_FLOAT_EQ(rawGener->particles[0].position.x(), 3.0f);
}

} // namespace
} // namespace wallpaper
