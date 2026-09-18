using System;
using UnityEngine;

namespace UnityRecoverManagedFixture.Alpha
{
    [Serializable]
    public sealed class Payload
    {
        public int marker;
        public string label;
        [SerializeReference] public object next;
    }
}

namespace UnityRecoverManagedFixture.Beta
{
    [Serializable]
    public sealed class Payload
    {
        public int marker;
        public float amount;
        [SerializeReference] public object next;
    }
}
