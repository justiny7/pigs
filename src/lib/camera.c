#include "camera.h"
#include "lib.h"
#include "math.h"

void init_camera(Camera* c, Vec3 pos, Vec3 target, Vec3 up, uint32_t width, uint32_t height) {
    c->pos = pos;
    c->target = target;
    c->up = up;
    c->width = width;
    c->height = height;

    float fov_y = 60.f * M_PI / 180.f;
    c->fy = (float) height / (2.f * tanf(fov_y / 2.f));
    c->fx = c->fy;
    c->cx = (float) width / 2.f;
    c->cy = (float) height / 2.f;

    Vec3 f = vec3_sub(target, pos);
    f = vec3_sdiv(f, vec3_len(f));
    Vec3 r = vec3_cross(f, up);
    r = vec3_sdiv(r, vec3_len(r));
    Vec3 u = vec3_cross(r, f);

    Mat4 w2c;
    w2c.m[0] = r.x;  w2c.m[1] = r.y;  w2c.m[2] = r.z;
    w2c.m[4] = u.x;  w2c.m[5] = u.y;  w2c.m[6] = u.z;
    w2c.m[8] = f.x;  w2c.m[9] = f.y;  w2c.m[10] = f.z;

    w2c.m[3]  = -(r.x * pos.x + r.y * pos.y + r.z * pos.z);
    w2c.m[7]  = -(u.x * pos.x + u.y * pos.y + u.z * pos.z);
    w2c.m[11] = -(f.x * pos.x + f.y * pos.y + f.z * pos.z);

    w2c.m[12] = 0.0f; w2c.m[13] = 0.0f; w2c.m[14] = 0.0f; w2c.m[15] = 1.0f;

    c->w2c = w2c;
}

void project_point(Camera* c, Vec3 p, float* depth, float* u, float* v) {
    float tx = c->w2c.m[0] * p.x + c->w2c.m[1] * p.y + c->w2c.m[2] * p.z + c->w2c.m[3];
    float ty = c->w2c.m[4] * p.x + c->w2c.m[5] * p.y + c->w2c.m[6] * p.z + c->w2c.m[7];
    float tz = c->w2c.m[8] * p.x + c->w2c.m[9] * p.y + c->w2c.m[10] * p.z + c->w2c.m[11];
    assert(tz != 0, "tz is zero");
    // assert(tz >= 0, "tz is neg");

    *depth = tz;
    if (tz >= 0 && tz < 0.001) {
        // panic("SMALL POS");
        return;
    } else if (tz <= 0 && tz > -0.001) {
        // panic("SMALL NEG");
        return;
    }

    *u = (c->fx * tx / tz) + c->cx;
    *v = (c->fy * ty / tz) + c->cy;
}

Mat3 quat_to_rotmat(Vec4 q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float xw = q.x * q.w, yw = q.y * q.w, zw = q.z * q.w;

    Mat3 res;
    res.m[0] = 1.0f - 2.0f * (yy + zz);
    res.m[1] = 2.0f * (xy - zw);
    res.m[2] = 2.0f * (xz + yw);
    res.m[3] = 2.0f * (xy + zw);
    res.m[4] = 1.0f - 2.0f * (xx + zz);
    res.m[5] = 2.0f * (yz - xw);
    res.m[6] = 2.0f * (xz - yw);
    res.m[7] = 2.0f * (yz + xw);
    res.m[8] = 1.0f - 2.0f * (xx + yy);

    return res;
}

