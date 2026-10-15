import bpy, math, mathutils
def setup_render(res=(768,1024), samples=24):
    s=bpy.context.scene
    s.render.engine='CYCLES'; s.cycles.device='CPU'; s.cycles.samples=samples
    s.cycles.use_denoising=True
    s.render.resolution_x, s.render.resolution_y = res; s.render.resolution_percentage=100
    s.render.film_transparent=False
    w=bpy.data.worlds.new("w"); s.world=w; w.use_nodes=True
    w.node_tree.nodes['Background'].inputs[0].default_value=(0.55,0.6,0.7,1); w.node_tree.nodes['Background'].inputs[1].default_value=0.6
    for n,rot,e in [("key",(50,0,35),4),("fill",(60,0,-120),1.5),("rim",(70,0,180),2.5)]:
        l=bpy.data.lights.new(n,'SUN'); l.energy=e; o=bpy.data.objects.new(n,l); s.collection.objects.link(o)
        o.rotation_euler=[math.radians(a) for a in rot]
    cam=bpy.data.cameras.new("cam"); co=bpy.data.objects.new("cam",cam); s.collection.objects.link(co); s.camera=co
    return co
def shoot(co, target, dist, az, el, path, lens=50):
    t=mathutils.Vector(target)
    d=mathutils.Vector((math.sin(math.radians(az))*math.cos(math.radians(el)), -math.cos(math.radians(az))*math.cos(math.radians(el)), math.sin(math.radians(el))))
    co.location=t+d*dist; co.data.lens=lens
    co.rotation_euler=(t-co.location).to_track_quat('-Z','Y').to_euler()
    bpy.context.scene.render.filepath=path; bpy.ops.render.render(write_still=True)
