using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Content;
using UnityEditor.Build.Player;
using UnityEngine;
using AlphaPayload = UnityRecoverManagedFixture.Alpha.Payload;
using BetaPayload = UnityRecoverManagedFixture.Beta.Payload;

public static class DirectoryFixtureBuilder
{
    private static readonly string[] CaseNames =
    {
        "null", "single", "shared", "two-types", "nested", "self-cycle",
        "two-cycle", "host-alpha", "host-beta"
    };

    [Serializable]
    public sealed class ReferenceObservation
    {
        public long id;
        public bool isNull;
        public string className;
        public string namespaceName;
        public string assemblyName;
        public int marker;
        public string label;
        public float amount;
        public long nextId;
        public bool nextResolvesToSameInstance;
    }

    [Serializable]
    public sealed class HostObservation
    {
        public string caseName;
        public string guid;
        public long pathId;
        public long firstId;
        public long secondId;
        public long[] itemIds;
        public bool firstAndSecondSame;
        public bool idsResolveToOriginalInstances;
        public long unknownUntrackedObjectId;
        public List<ReferenceObservation> references = new List<ReferenceObservation>();
    }

    [Serializable]
    public sealed class WrittenObject
    {
        public string guid;
        public long pathId;
        public ulong offset;
        public ulong size;
    }

    [Serializable]
    public sealed class Variant
    {
        public string flags;
        public List<WrittenObject> objects = new List<WrittenObject>();
        public List<string> includedTypes = new List<string>();
    }

    [Serializable]
    public sealed class Report
    {
        public string engineVersion;
        public string target;
        public long nullId;
        public long unknownId;
        public bool observationsAfterAssetReload;
        public string status;
        public string error;
        public List<HostObservation> hosts = new List<HostObservation>();
        public List<Variant> variants = new List<Variant>();
    }

    public static void Run()
    {
        string output = Environment.GetEnvironmentVariable("UNITYCOMMON_FIXTURE_OUTPUT");
        string targetName = Environment.GetEnvironmentVariable("UNITYCOMMON_FIXTURE_TARGET");
        Report report = new Report();
        report.engineVersion = Application.unityVersion;
        report.target = targetName;
        report.nullId = UnityEditor.SerializationUtility.RefIdNull;
        report.unknownId = UnityEditor.SerializationUtility.RefIdUnknown;
        int exitCode = 1;
        try
        {
            if (Application.unityVersion != "2021.3.35f1")
                throw new InvalidOperationException("The fixture requires the pinned Editor.");
            BuildTarget target = (BuildTarget)Enum.Parse(typeof(BuildTarget), targetName);
            Directory.CreateDirectory(output);
            CreateAssets();
            report.observationsAfterAssetReload = true;
            string retainedInputs = Path.Combine(output, "source-assets");
            Directory.CreateDirectory(retainedInputs);
            foreach (string path in Directory.GetFiles("Assets", "*", SearchOption.TopDirectoryOnly))
                File.Copy(path, Path.Combine(retainedInputs, Path.GetFileName(path)), true);
            foreach (string caseName in CaseNames)
                report.hosts.Add(ObserveHost(caseName));

            ScriptCompilationSettings compilation = new ScriptCompilationSettings();
            compilation.target = target;
            compilation.group = BuildTargetGroup.Standalone;
            compilation.options = ScriptCompilationOptions.None;
            ScriptCompilationResult compiled = PlayerBuildInterface.CompilePlayerScripts(
                compilation, Path.GetFullPath("FixtureCompiled"));
            if (compiled.typeDB == null)
                throw new InvalidOperationException("No TypeDB was returned.");
            using (TypeDB typeDB = compiled.typeDB)
            {
                report.variants.Add(WriteVariant(output, target, typeDB,
                    ContentBuildFlags.None, "with-tree"));
                report.variants.Add(WriteVariant(output, target, typeDB,
                    ContentBuildFlags.DisableWriteTypeTree, "without-tree"));
            }
            report.status = "success";
            exitCode = 0;
        }
        catch (Exception exception)
        {
            report.status = "failure";
            report.error = exception.ToString();
            Debug.LogException(exception);
        }
        finally
        {
            Directory.CreateDirectory(output);
            File.WriteAllText(Path.Combine(output, "writer-report.json"),
                JsonUtility.ToJson(report, true));
            EditorApplication.Exit(exitCode);
        }
    }

    private static string AssetPath(string caseName)
    {
        return "Assets/" + caseName + ".asset";
    }

    private static long PathId(string caseName)
    {
        int ordinal = Array.IndexOf(CaseNames, caseName);
        if (ordinal < 0)
            throw new InvalidOperationException("Unknown fixture case.");
        return 1001 + 2 * ordinal;
    }

    private static void AssignId(RegistryFixtureHost host, object value, long id)
    {
        if (!UnityEditor.SerializationUtility.SetManagedReferenceIdForObject(host, value, id))
            throw new InvalidOperationException("Could not assign an explicit reference ID.");
    }

    private static void CreateAssets()
    {
        foreach (string caseName in CaseNames)
        {
            RegistryFixtureHost host = ScriptableObject.CreateInstance<RegistryFixtureHost>();
            host.caseTag = caseName;
            host.items = new object[0];
            AssetDatabase.CreateAsset(host, AssetPath(caseName));
            AlphaPayload alpha = new AlphaPayload { marker = 1011, label = "alpha payload" };
            BetaPayload beta = new BetaPayload { marker = 2022, amount = 1.25f };

            if (caseName == "single")
                host.first = alpha;
            else if (caseName == "shared")
            {
                host.first = alpha;
                host.second = alpha;
                host.items = new object[] { alpha, null, alpha };
            }
            else if (caseName == "two-types")
            {
                host.first = alpha;
                host.second = beta;
                host.items = new object[] { beta, alpha };
            }
            else if (caseName == "nested")
            {
                host.first = alpha;
                alpha.next = beta;
            }
            else if (caseName == "self-cycle")
            {
                host.first = alpha;
                alpha.next = alpha;
            }
            else if (caseName == "two-cycle")
            {
                host.first = alpha;
                alpha.next = beta;
                beta.next = alpha;
                host.second = beta;
            }
            else if (caseName == "host-alpha")
            {
                alpha.marker = 3033;
                alpha.label = "same id host alpha";
                host.first = alpha;
            }
            else if (caseName == "host-beta")
            {
                beta.marker = 4044;
                beta.amount = -2.5f;
                host.first = beta;
            }

            if (host.first == alpha)
                AssignId(host, alpha, caseName == "single" ? 6441160360502231040L : 101L);
            if (host.second == beta || alpha.next == beta)
                AssignId(host, beta, 202L);
            if (caseName == "host-beta")
                AssignId(host, beta, 101L);
            EditorUtility.SetDirty(host);
        }
        AssetDatabase.SaveAssets();
        foreach (string caseName in CaseNames)
        {
            RegistryFixtureHost host = AssetDatabase.LoadAssetAtPath<RegistryFixtureHost>(AssetPath(caseName));
            Resources.UnloadAsset(host);
            AssetDatabase.ImportAsset(AssetPath(caseName),
                ImportAssetOptions.ForceSynchronousImport | ImportAssetOptions.ForceUpdate);
        }
        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
    }

    private static HostObservation ObserveHost(string caseName)
    {
        RegistryFixtureHost host = AssetDatabase.LoadAssetAtPath<RegistryFixtureHost>(AssetPath(caseName));
        if (host == null)
            throw new InvalidOperationException("Could not load a fixture host.");
        HostObservation result = new HostObservation();
        result.caseName = caseName;
        result.guid = AssetDatabase.AssetPathToGUID(AssetPath(caseName));
        result.pathId = PathId(caseName);
        result.firstId = Id(host, host.first);
        result.secondId = Id(host, host.second);
        result.itemIds = new long[host.items.Length];
        for (int index = 0; index < host.items.Length; ++index)
            result.itemIds[index] = Id(host, host.items[index]);
        result.firstAndSecondSame = ReferenceEquals(host.first, host.second);
        result.idsResolveToOriginalInstances = Resolves(host, host.first) && Resolves(host, host.second);
        result.unknownUntrackedObjectId =
            UnityEditor.SerializationUtility.GetManagedReferenceIdForObject(host, new object());
        foreach (object value in host.items)
            result.idsResolveToOriginalInstances &= Resolves(host, value);

        long[] ids = UnityEditor.SerializationUtility.GetManagedReferenceIds(host);
        Array.Sort(ids);
        foreach (long id in ids)
        {
            object value = UnityEditor.SerializationUtility.GetManagedReference(host, id);
            ReferenceObservation observation = new ReferenceObservation();
            observation.id = id;
            observation.isNull = value == null;
            if (value == null)
            {
                if (id != UnityEditor.SerializationUtility.RefIdNull)
                    throw new InvalidOperationException("A non-null reference did not resolve.");
                result.references.Add(observation);
                continue;
            }
            Type type = value.GetType();
            observation.className = type.Name;
            observation.namespaceName = type.Namespace;
            observation.assemblyName = type.Assembly.GetName().Name;
            object next;
            AlphaPayload alpha = value as AlphaPayload;
            BetaPayload beta = value as BetaPayload;
            if (alpha != null)
            {
                observation.marker = alpha.marker;
                observation.label = alpha.label;
                next = alpha.next;
            }
            else if (beta != null)
            {
                observation.marker = beta.marker;
                observation.amount = beta.amount;
                next = beta.next;
            }
            else
                throw new InvalidOperationException("Unexpected fixture payload type.");
            observation.nextId = Id(host, next);
            observation.nextResolvesToSameInstance = Resolves(host, next);
            result.references.Add(observation);
        }
        return result;
    }

    private static long Id(RegistryFixtureHost host, object value)
    {
        return value == null ? UnityEditor.SerializationUtility.RefIdNull :
            UnityEditor.SerializationUtility.GetManagedReferenceIdForObject(host, value);
    }

    private static bool Resolves(RegistryFixtureHost host, object value)
    {
        return value == null || ReferenceEquals(value,
            UnityEditor.SerializationUtility.GetManagedReference(host, Id(host, value)));
    }

    private static Variant WriteVariant(string output, BuildTarget target, TypeDB typeDB,
        ContentBuildFlags flags, string variantName)
    {
        string destination = Path.Combine(output, variantName);
        Directory.CreateDirectory(destination);
        const string internalName = "managed-reference-fixture.assets";
        List<SerializationInfo> primary = new List<SerializationInfo>();
        foreach (string caseName in CaseNames)
        {
            GUID guid = new GUID(AssetDatabase.AssetPathToGUID(AssetPath(caseName)));
            ObjectIdentifier[] objects = ContentBuildInterface.GetPlayerObjectIdentifiersInAsset(guid, target);
            if (objects.Length != 1)
                throw new InvalidOperationException("Expected one primary object per fixture host.");
            SerializationInfo entry = new SerializationInfo();
            entry.serializationObject = objects[0];
            entry.serializationIndex = PathId(caseName);
            primary.Add(entry);
        }
        ObjectIdentifier[] primaryIdentifiers = new ObjectIdentifier[primary.Count];
        for (int index = 0; index < primary.Count; ++index)
            primaryIdentifiers[index] = primary[index].serializationObject;
        ObjectIdentifier[] dependencies = ContentBuildInterface.GetPlayerDependenciesForObjects(
            primaryIdentifiers, target, typeDB);
        if (dependencies.Length > 32)
            throw new InvalidOperationException("Dependency observation cap exceeded.");
        List<SerializationInfo> external = new List<SerializationInfo>();
        HashSet<ObjectIdentifier> seen = new HashSet<ObjectIdentifier>(primaryIdentifiers);
        foreach (ObjectIdentifier dependency in dependencies)
        {
            if (!seen.Add(dependency))
                continue;
            if (ContentBuildInterface.GetTypeForObject(dependency) != typeof(MonoScript))
                throw new InvalidOperationException("Unexpected external dependency.");
            SerializationInfo entry = new SerializationInfo();
            entry.serializationObject = dependency;
            entry.serializationIndex = 9001 + external.Count;
            external.Add(entry);
        }

        using (BuildReferenceMap references = new BuildReferenceMap())
        using (BuildUsageTagSet usage = new BuildUsageTagSet())
        {
            references.AddMappings(internalName, primary.ToArray());
            references.AddMappings("managed-reference-scripts.assets", external.ToArray());
            WriteCommand command = new WriteCommand();
            command.fileName = internalName;
            command.internalName = internalName;
            command.serializeObjects = primary;
            BuildSettings settings = new BuildSettings();
            settings.target = target;
            settings.group = BuildTargetGroup.Standalone;
            settings.typeDB = typeDB;
            settings.buildFlags = flags;
            WriteParameters parameters = new WriteParameters();
            parameters.writeCommand = command;
            parameters.settings = settings;
            parameters.usageSet = usage;
            parameters.referenceMap = references;
            WriteResult written = ContentBuildInterface.WriteSerializedFile(destination, parameters);
            Variant result = new Variant();
            result.flags = flags.ToString();
            foreach (ObjectSerializedInfo row in written.serializedObjects)
            {
                WrittenObject observation = new WrittenObject();
                observation.guid = row.serializedObject.guid.ToString();
                foreach (SerializationInfo requested in primary)
                {
                    if (requested.serializationObject == row.serializedObject)
                        observation.pathId = requested.serializationIndex;
                }
                observation.offset = row.header.offset;
                observation.size = row.header.size;
                result.objects.Add(observation);
            }
            foreach (Type type in written.includedTypes)
                result.includedTypes.Add(type.AssemblyQualifiedName);
            foreach (ResourceFile file in written.resourceFiles)
            {
                if (new FileInfo(file.fileName).Length > 1024 * 1024)
                    throw new InvalidOperationException("Writer output cap exceeded.");
            }
            return result;
        }
    }
}
