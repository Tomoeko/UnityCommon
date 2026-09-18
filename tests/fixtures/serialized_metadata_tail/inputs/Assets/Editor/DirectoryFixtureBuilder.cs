using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Content;
using UnityEditor.Build.Player;
using UnityEngine;

public static class DirectoryFixtureBuilder
{
    [Serializable]
    public sealed class Identity
    {
        public string guid;
        public long localIdentifier;
        public string fileType;
        public string filePath;
        public string managedType;
        public long requestedSerializationIndex;
    }

    [Serializable]
    public sealed class Location
    {
        public string fileName;
        public ulong offset;
        public ulong size;
    }

    [Serializable]
    public sealed class WrittenObject
    {
        public Identity identity;
        public Location header;
        public Location rawData;
    }

    [Serializable]
    public sealed class WrittenFile
    {
        public string path;
        public string alias;
        public bool serializedFile;
    }

    [Serializable]
    public sealed class Variant
    {
        public string flags;
        public List<WrittenObject> objects = new List<WrittenObject>();
        public List<WrittenFile> files = new List<WrittenFile>();
        public List<string> includedTypes = new List<string>();
    }

    [Serializable]
    public sealed class Report
    {
        public string engineVersion;
        public string target;
        public string group;
        public string status;
        public string error;
        public List<Identity> requestedObjects = new List<Identity>();
        public List<Variant> variants = new List<Variant>();
    }

    public static void Run()
    {
        string output = Environment.GetEnvironmentVariable("UNITYCOMMON_FIXTURE_OUTPUT");
        string targetName = Environment.GetEnvironmentVariable("UNITYCOMMON_FIXTURE_TARGET");
        Report report = new Report();
        report.engineVersion = Application.unityVersion;
        report.target = targetName;
        report.group = BuildTargetGroup.Standalone.ToString();
        int exitCode = 1;

        try
        {
            if (Application.unityVersion != "2021.3.35f1")
                throw new InvalidOperationException("The fixture requires the exact pinned Editor.");
            BuildTarget target = (BuildTarget)Enum.Parse(typeof(BuildTarget), targetName);
            Directory.CreateDirectory(output);
            CreateAssets();

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
                List<ObjectIdentifier> objects = CollectObjects(target, typeDB);
                List<SerializationInfo> serialization = new List<SerializationInfo>();
                for (int ordinal = 0; ordinal < objects.Count; ++ordinal)
                {
                    SerializationInfo entry = new SerializationInfo();
                    entry.serializationObject = objects[ordinal];
                    entry.serializationIndex = 1001 + 1002 * ordinal;
                    serialization.Add(entry);
                    report.requestedObjects.Add(Describe(objects[ordinal], entry.serializationIndex));
                }

                report.variants.Add(WriteVariant(output, target, typeDB, serialization,
                    ContentBuildFlags.None, "with-tree"));
                report.variants.Add(WriteVariant(output, target, typeDB, serialization,
                    ContentBuildFlags.DisableWriteTypeTree, "without-tree"));
            }

            string retainedInputs = Path.Combine(output, "source-assets");
            Directory.CreateDirectory(retainedInputs);
            foreach (string path in Directory.GetFiles("Assets", "*", SearchOption.TopDirectoryOnly))
                File.Copy(path, Path.Combine(retainedInputs, Path.GetFileName(path)), true);
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
            File.WriteAllText(Path.Combine(output, "writer-report.json"), JsonUtility.ToJson(report, true));
            EditorApplication.Exit(exitCode);
        }
    }

    private static void CreateAssets()
    {
        File.WriteAllText("Assets/DirectoryFixture.txt", "native fixture marker 37\n");
        DirectoryFixtureA first = ScriptableObject.CreateInstance<DirectoryFixtureA>();
        first.marker = 0x13579;
        AssetDatabase.CreateAsset(first, "Assets/DirectoryFixtureA.asset");
        DirectoryFixtureB second = ScriptableObject.CreateInstance<DirectoryFixtureB>();
        second.marker = 0x24680;
        AssetDatabase.CreateAsset(second, "Assets/DirectoryFixtureB.asset");
        first.external = second;
        first.payload = new UnityRecoverTailFixture.TailPayload
        {
            marker = 0x35791,
            label = "tail payload marker"
        };
        // Preserve the controlled original payload identity instead of allowing
        // Unity to generate a different managed-reference ID on every run.
        const long payloadReferenceId = 6441160360502231040L;
        if (!UnityEditor.SerializationUtility.SetManagedReferenceIdForObject(
            first, first.payload, payloadReferenceId))
            throw new InvalidOperationException("Could not assign the fixed fixture payload ID.");
        EditorUtility.SetDirty(first);
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
    }

    private static List<ObjectIdentifier> CollectObjects(BuildTarget target, TypeDB typeDB)
    {
        List<ObjectIdentifier> objects = new List<ObjectIdentifier>();
        HashSet<ObjectIdentifier> seen = new HashSet<ObjectIdentifier>();
        string[] paths = { "Assets/DirectoryFixture.txt", "Assets/DirectoryFixtureA.asset",
            "Assets/DirectoryFixtureB.asset" };
        foreach (string path in paths)
        {
            ObjectIdentifier[] identifiers = ContentBuildInterface.GetPlayerObjectIdentifiersInAsset(
                new GUID(AssetDatabase.AssetPathToGUID(path)), target);
            if (identifiers.Length != 1)
                throw new InvalidOperationException("Expected exactly one primary object per fixture asset.");
            foreach (ObjectIdentifier identifier in identifiers)
            {
                if (seen.Add(identifier))
                    objects.Add(identifier);
            }
        }

        ObjectIdentifier[] dependencies = ContentBuildInterface.GetPlayerDependenciesForObjects(
            objects.ToArray(), target, typeDB);
        if (dependencies.Length > 16)
            throw new InvalidOperationException("Fixture dependency count exceeded its bound.");
        foreach (ObjectIdentifier dependency in dependencies)
        {
            if (seen.Contains(dependency))
                continue;
            if (ContentBuildInterface.GetTypeForObject(dependency) != typeof(MonoScript))
                throw new InvalidOperationException("Unexpected fixture dependency: " + dependency);
            seen.Add(dependency);
            objects.Add(dependency);
        }
        if (objects.Count > 16)
            throw new InvalidOperationException("Fixture object count exceeded its bound.");
        return objects;
    }

    private static Variant WriteVariant(string output, BuildTarget target, TypeDB typeDB,
        List<SerializationInfo> serialization, ContentBuildFlags flags, string name)
    {
        string destination = Path.Combine(output, name);
        Directory.CreateDirectory(destination);
        const string internalName = "directory-fixture.assets";

        using (BuildReferenceMap references = new BuildReferenceMap())
        using (BuildUsageTagSet usage = new BuildUsageTagSet())
        {
            List<SerializationInfo> primary = new List<SerializationInfo>();
            List<SerializationInfo> external = new List<SerializationInfo>();
            foreach (SerializationInfo entry in serialization)
            {
                Type objectType = ContentBuildInterface.GetTypeForObject(entry.serializationObject);
                if (objectType == typeof(TextAsset) || objectType == typeof(DirectoryFixtureA))
                    primary.Add(entry);
                else
                    external.Add(entry);
            }
            if (primary.Count != 2 || external.Count < 3 || external.Count > 8)
                throw new InvalidOperationException("Unexpected partition cardinality.");
            references.AddMappings(internalName, primary.ToArray());
            references.AddMappings("tail-external.assets", external.ToArray());
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

            WriteResult result = ContentBuildInterface.WriteSerializedFile(destination, parameters);
            Variant variant = new Variant();
            variant.flags = flags.ToString();
            foreach (ObjectSerializedInfo written in result.serializedObjects)
            {
                long index = -1;
                foreach (SerializationInfo requested in serialization)
                {
                    if (requested.serializationObject == written.serializedObject)
                        index = requested.serializationIndex;
                }
                WrittenObject row = new WrittenObject();
                row.identity = Describe(written.serializedObject, index);
                row.header = Describe(written.header);
                row.rawData = Describe(written.rawData);
                variant.objects.Add(row);
            }
            foreach (ResourceFile file in result.resourceFiles)
            {
                WrittenFile entry = new WrittenFile();
                entry.path = file.fileName;
                entry.alias = file.fileAlias;
                entry.serializedFile = file.serializedFile;
                variant.files.Add(entry);
                if (new FileInfo(file.fileName).Length > 1024 * 1024)
                    throw new InvalidOperationException("Writer output exceeded the one MiB fixture bound.");
            }
            foreach (Type type in result.includedTypes)
                variant.includedTypes.Add(type.AssemblyQualifiedName);
            return variant;
        }
    }

    private static Identity Describe(ObjectIdentifier identifier, long requestedIndex)
    {
        Identity result = new Identity();
        result.guid = identifier.guid.ToString();
        result.localIdentifier = identifier.localIdentifierInFile;
        result.fileType = identifier.fileType.ToString();
        result.filePath = identifier.filePath;
        Type type = ContentBuildInterface.GetTypeForObject(identifier);
        result.managedType = type == null ? null : type.AssemblyQualifiedName;
        result.requestedSerializationIndex = requestedIndex;
        return result;
    }

    private static Location Describe(SerializedLocation location)
    {
        Location result = new Location();
        result.fileName = location.fileName;
        result.offset = location.offset;
        result.size = location.size;
        return result;
    }
}
