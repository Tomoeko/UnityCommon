using UnityEngine;

public sealed class RegistryFixtureHost : ScriptableObject
{
    public string caseTag;
    [SerializeReference] public object first;
    [SerializeReference] public object second;
    [SerializeReference] public object[] items;
}
