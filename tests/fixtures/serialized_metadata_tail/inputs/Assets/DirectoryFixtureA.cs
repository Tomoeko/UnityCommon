using UnityEngine;

public sealed class DirectoryFixtureA : ScriptableObject
{
    public int marker;
    public UnityEngine.Object external;
    [SerializeReference] public object payload;
}
