// The first registry modules: the same eleven things game.dll can do through TbdGameHost, now callable by name.
// To expose a new engine feature, add a Reg(...) block here (or in a new api_<area>.cpp) and nothing else.
#include <cstdio>
#include <string_view>

#include "../game_api.h"
#include "api_registry.h"

namespace tbd::script {

namespace {

const TbdGameHost* g_host = nullptr;

void Reg(const char* module, const char* name, const char* since, const char* doc, std::vector<Param> params, const char* returns, ApiFn fn,
         Avail avail = Avail::Always) {
    ApiFunction f;
    f.module = module;
    f.name = name;
    f.since = since;
    f.doc = doc;
    f.params = std::move(params);
    f.returns = returns;
    f.avail = avail;
    f.fn = std::move(fn);
    Registry::Get().Add(std::move(f));
}

int Idx(const Value& v) { return v.type == VType::Number ? (int)v.n : -1; }

} // namespace

void RegisterBuiltinApi(const TbdGameHost* host) {
    g_host = host;
    const TbdGameHost* h = host;

    // ---- log
    Reg("log", "info", "1.0", "Writes a line to bin/engine.log and the editor console.", {{"text", "string"}}, "", [h](auto& a, Value&, std::string&) {
        h->log(a[0].s.c_str());
        return true;
    });

    // ---- scene (entities are indices; Play undoes every move made here)
    Reg("scene", "count", "1.0", "Number of entities in the scene.", {}, "number", [h](auto&, Value& out, std::string&) {
        out = Value::Num(h->entityCount());
        return true;
    });
    Reg("scene", "find", "1.0", "Index of the entity with this exact name, or nil.", {{"name", "string"}}, "entity", [h](auto& a, Value& out, std::string&) {
        const int i = h->findEntity(a[0].s.c_str());
        out = i >= 0 ? Value::Num(i) : Value::Nil();
        return true;
    });
    Reg("scene", "find_all", "1.0", "Indices of every entity whose name starts with the prefix (for example \"Doors/\").", {{"prefix", "string"}}, "number[]",
        [h](auto& a, Value& out, std::string&) {
            out = Value();
            out.type = VType::Array;
            const int n = h->entityCount();
            for (int i = 0; i < n; ++i) {
                const char* name = h->entityName(i);
                if (name && std::string_view(name).substr(0, a[0].s.size()) == a[0].s) out.a.push_back(Value::Num(i));
            }
            return true;
        });
    Reg("scene", "name", "1.0", "Name of an entity, or an empty string for a bad index.", {{"entity", "entity"}}, "string", [h](auto& a, Value& out, std::string&) {
        out = Value::Str(h->entityName(Idx(a[0])));
        return true;
    });
    Reg("scene", "position", "1.0", "World position of an entity.", {{"entity", "entity"}}, "vec3", [h](auto& a, Value& out, std::string&) {
        float p[3] = {}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        h->getTransform(Idx(a[0]), p, q, s);
        out = Value::Vec(p[0], p[1], p[2]);
        return true;
    });
    Reg("scene", "rotation", "1.0", "Rotation of an entity as a quaternion.", {{"entity", "entity"}}, "quat", [h](auto& a, Value& out, std::string&) {
        float p[3] = {}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        h->getTransform(Idx(a[0]), p, q, s);
        out = Value::Rot(q[0], q[1], q[2], q[3]);
        return true;
    });
    Reg("scene", "scale", "1.0", "Scale of an entity.", {{"entity", "entity"}}, "vec3", [h](auto& a, Value& out, std::string&) {
        float p[3] = {}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        h->getTransform(Idx(a[0]), p, q, s);
        out = Value::Vec(s[0], s[1], s[2]);
        return true;
    });
    // Setters read the other two parts first so a script can change one without knowing the rest.
    Reg("scene", "set_position", "1.0", "Moves an entity. Only takes effect in Play; Stop puts it back.", {{"entity", "entity"}, {"position", "vec3"}}, "",
        [h](auto& a, Value&, std::string&) {
            float p[3], q[4], s[3];
            h->getTransform(Idx(a[0]), p, q, s);
            h->setTransform(Idx(a[0]), a[1].v, q, s);
            return true;
        }, Avail::PlayOnly);
    Reg("scene", "set_rotation", "1.0", "Rotates an entity (quaternion x y z w). Only takes effect in Play.", {{"entity", "entity"}, {"rotation", "quat"}}, "",
        [h](auto& a, Value&, std::string&) {
            float p[3], q[4], s[3];
            h->getTransform(Idx(a[0]), p, q, s);
            h->setTransform(Idx(a[0]), p, a[1].v, s);
            return true;
        }, Avail::PlayOnly);
    Reg("scene", "set_scale", "1.0", "Scales an entity. Only takes effect in Play.", {{"entity", "entity"}, {"scale", "vec3"}}, "",
        [h](auto& a, Value&, std::string&) {
            float p[3], q[4], s[3];
            h->getTransform(Idx(a[0]), p, q, s);
            h->setTransform(Idx(a[0]), p, q, a[1].v);
            return true;
        }, Avail::PlayOnly);

    // ---- input
    Reg("input", "down", "1.0", "True while a key is held, in Play. Key is a Windows virtual-key code or the character code of 'A'..'Z'.", {{"key", "number"}}, "bool",
        [h](auto& a, Value& out, std::string&) {
            out = Value::Bool(h->keyDown((int)a[0].n) != 0);
            return true;
        });

    // ---- player
    Reg("player", "feet", "1.0", "Position of the walking player's feet, or nil when there is none.", {}, "vec3?", [h](auto&, Value& out, std::string&) {
        float p[3];
        out = h->playerFeet(p) ? Value::Vec(p[0], p[1], p[2]) : Value::Nil();
        return true;
    });

    // ---- world
    Reg("world", "hour", "1.0", "Time of day in hours, 0..24.", {}, "number", [h](auto&, Value& out, std::string&) {
        out = Value::Num(h->getHour());
        return true;
    });
    Reg("world", "set_hour", "1.0", "Sets the time of day in hours, 0..24.", {{"hour", "number"}}, "", [h](auto& a, Value&, std::string&) {
        h->setHour((float)a[0].n);
        return true;
    });

    // ---- character (the character creator)
    Reg("character", "set_skin_detail_scale", "1.1",
        "How many times the skin pore detail normal repeats across this entity's skin UV atlas (default 48; 0 or less resets to 48, 1 = untiled). "
        "Returns false for a bad entity index.",
        {{"entity", "entity"}, {"scale", "number"}}, "bool", [](auto& a, Value& out, std::string& err) {
            if (!Services().setSkinDetailScale) { err = "not available in this build"; return false; }
            out = Value::Bool(Services().setSkinDetailScale(Idx(a[0]), (float)a[1].n));
            return true;
        });
    Reg("character", "set_skin_pore_depth", "1.2",
        "How deep this entity's skin pores read: 1 default, 0.5 young and fine, 2 old and coarse (clamped to 3); below 0 resets to 1. "
        "Returns false for a bad entity index.",
        {{"entity", "entity"}, {"depth", "number"}}, "bool", [](auto& a, Value& out, std::string& err) {
            if (!Services().setSkinPoreDepth) { err = "not available in this build"; return false; }
            out = Value::Bool(Services().setSkinPoreDepth(Idx(a[0]), (float)a[1].n));
            return true;
        });

    // ---- audio
    Reg("audio", "play", "1.0", "Plays a one-shot sound at a world position. Volume and pitch default to 1.",
        {{"name", "string"}, {"position", "vec3"}, {"volume", "number?"}, {"pitch", "number?"}}, "", [h](auto& a, Value&, std::string&) {
            h->playSound(a[0].s.c_str(), a[1].v, a.size() > 2 && a[2].type == VType::Number ? (float)a[2].n : 1.0f,
                         a.size() > 3 && a[3].type == VType::Number ? (float)a[3].n : 1.0f);
            return true;
        });
}

} // namespace tbd::script
