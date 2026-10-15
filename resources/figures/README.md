# Figures

The people the 3D reference panel offers under *Figures*, beside the jointed
mannequin. Each is a GLB of plain meshes with, beside it, a `.json` of the
same name holding the name shown and the 22 joints it is rigged with (x, y, z
each, in the order `Model3D::rigJointNames()` gives, in the model's own units).

To add one, rig the model in Impasto (Open, then Rig, put any joint right,
Bind), then make the pair from it and list both files in `resources.qrc`:

```bash
./build/impasto --make-figure model.obj resources/figures/name.glb "Name" ~/.config/Impasto/Impasto.conf
```

The settings file is where the Rig button kept the joints; without it the
joints are fitted afresh. A figure kept with *Keep* in the panel goes to
`~/.local/share/Impasto/Impasto/figures` in the same form, and shows without
rebuilding anything.

- `woman.glb`: a stylised woman, made by ozzy-apps with Meshy AI.
