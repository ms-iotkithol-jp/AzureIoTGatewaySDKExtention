using System;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Runtime.Serialization.Json;
using Microsoft.WindowsAzure.Storage;
using Microsoft.WindowsAzure.Storage.Blob;
using Newtonsoft.Json;
using Newtonsoft.Json.Serialization;
using System.IO;

namespace remoteupdateutil
{
    class Program
    {
        static void Main(string[] args)
        {
            if (args[0]=="-h" ||args[0]=="--help")
            {
                Console.WriteLine("dotnet remoteupdateutil --storage-cs \"<connection string for storage account>\" -c <blob container name> -dp <deploy config file path on edge> -dv <deploy version> [-m <module name> -ln <loader name> -ml <module library path on build environmnent> -mp <library path on edge> -mv <module version> [-man <module argument name> -mav \"<module argument value>\"]] -et <sas token for library blob access expiration days> -om <modules config file name> -ot <twin deploy config file name>");
                Console.WriteLine("    or ");
                Console.WriteLine("dotnet remoteupdateutil --storage-cs \"<connection string for storage account>\" -c <blob container name> -dp <deploy config file path on edge> -dv <deploy version> --iothub-cs <connection string for IoT Hub> --work-directory <work directory for local config json file> -json <local config json file> -et <sas token for library blob access expiration days> -om <modules config file name> -ot <twin deploy config file name>");
            }
            DeployInfo di = new DeployInfo();
            di.gateway=new GatewayInfo();
            di.modules = new List<ModuleInfo>();
            di.links=new List<LinkInfo>();
            Dictionary<string, string> libs = new Dictionary<string,string>();
            var configInfo = new ConfigInfo();
            int argIndex = 0;
            var p = new Program();
            bool isConfigJson=false;
            while(argIndex<args.Length)
            {
                if (args[argIndex]=="--storage-cs"||args[argIndex]=="-scs")
                {
                    // connection string for storage account
                    argIndex++;
                    configInfo.StorageConnectionString = args[argIndex++];
                    if (configInfo.StorageConnectionString.StartsWith("\"")&&configInfo.StorageConnectionString.EndsWith("\""))
                        configInfo.StorageConnectionString = configInfo.StorageConnectionString.Substring(1,configInfo.StorageConnectionString.Length-2);
                }
                else if (args[argIndex]=="--iothub-cs"||args[argIndex]=="-ics"){
                    argIndex++;
                    configInfo.IoTHubConnectionString=args[argIndex++];
                    if (configInfo.IoTHubConnectionString.StartsWith("\"")&&configInfo.IoTHubConnectionString.EndsWith("\"")){
                        configInfo.IoTHubConnectionString=configInfo.IoTHubConnectionString.Substring(1,configInfo.IoTHubConnectionString.Length-2);
                    }
                }
                else if (args[argIndex]=="--iothub-transport"||args[argIndex]=="-it"){
                    argIndex++;
                    configInfo.IoTHubTransport = args[argIndex++];
                }
                else if (args[argIndex]=="--work-dir"||args[argIndex]=="-wd"){
                    argIndex++;
                    configInfo.CurrentWorkDirectory=args[argIndex++];
                }
                else if (args[argIndex]=="-bc"||args[argIndex]=="--blob-container")
                {
                    argIndex++;
                    configInfo.BlobContainerName=args[argIndex++];
                }
                else if (args[argIndex]=="-edp"||args[argIndex]=="--edge-deploy-path")
                {
                    argIndex++;
                    di.gateway.deployPath=args[argIndex++];
                }
                else if (args[argIndex]=="-dv"||args[argIndex]=="--deploy-version")
                {
                    // version for deploy
                    argIndex++;
                    di.gateway.version = args[argIndex++];
                }
                else if (args[argIndex]=="-ljcf"||args[argIndex]=="--local-json-config-file")
                {
                    argIndex++;
                    configInfo.LocalJsonConfigFile = args[argIndex++];
                    isConfigJson=true;

                }
                else if (args[argIndex]=="-ecjf"|| args[argIndex]=="--edge-config-json-file"){
                    argIndex++;
                    configInfo.EdgeConfigJsonFileName = args[argIndex++];
                }
                else if (args[argIndex]=="--module" ||args[argIndex]=="-m"){
                    argIndex++;
                    ModuleInfo mi = new ModuleInfo();
                    mi.name = args[argIndex++];
                    mi.args=new List<KeyValuePair<string,string>>();
                    mi.loader=new LoaderInfo();
                    mi.loader.entrypoint=new EntryPointInfo();
                    di.modules.Add(mi);
                    while (argIndex<args.Length){
                        if (args[argIndex]=="--loader-name" || args[argIndex]=="-ln"){
                            // loader.name
                            argIndex++;
                            mi.loader.name=args[argIndex++];
                        }
                        else if (args[argIndex]=="--module-library"|| args[argIndex]=="-ml"){
                            // path of library which will be uploaded to blob
                            argIndex++;
                            libs.Add(mi.name,args[argIndex++]);
                        }
                        else if (args[argIndex]=="--module-edge-path"|| args[argIndex]=="-mep"){
                            argIndex++;
                            mi.loader.entrypoint.modulePath=args[argIndex++];
                        }
                        else if (args[argIndex]=="--module-arg-name"|| args[argIndex]=="-man"){
                            // module argument name
                            argIndex++;
                            argIndex++;
                            if (args[argIndex]=="--module-arg-value"|| args[argIndex]=="-mav"){
                                // module argument value
                                // should be just after -man
                                argIndex++;
                                mi.args.Add(new KeyValuePair<string,string>(args[argIndex-2],args[argIndex]));
                                argIndex++;
                            }
                        }
                        else if(args[argIndex]=="--module-version"|| args[argIndex]=="-mv"){
                            // version of module
                            argIndex++;
                            mi.version=args[argIndex++];
                        }
                        else{
                            break;
                        }
                    }
                }
                else if (args[argIndex]=="--blob-module-config-file"|| args[argIndex] =="-om"){
                    // modules config file name
                    argIndex++;
                    configInfo.ModuleConfigJsonFileName=args[argIndex++];
                }
                else if (args[argIndex]=="--twin-config-file"||args[argIndex]=="-ot"){
                    // device twin config file name
                    argIndex++;
                    configInfo.TwinConfigJsonFileName=args[argIndex++];
                } else if (args[argIndex]=="--expire-duration"|| args[argIndex]=="-ed"){
                    // duration for end time of expiration url of blob
                    argIndex++;
                    configInfo.DayDuration= double.Parse(args[argIndex++]);
                } else if (args[argIndex]=="--link" || args[argIndex]=="-l"){
                    var link = new LinkInfo();
                    argIndex++;
                    var lss = args[argIndex++].Split(":");
                    link.source=lss[0];
                    link.sink=lss[1];
                    di.links.Add(link);
                }
                else {
                    break;
                }
            }
            try{
                if (isConfigJson){
                    p.ParseJsonConfig(configInfo, di,libs);
                }
                p.Generate(configInfo,di,libs).Wait();
            }
            catch (Exception ex){
                Console.WriteLine(ex.Message);
                if (ex is System.AggregateException){
                    var aEx = ex as System.AggregateException;
                    foreach(var cEx in aEx.InnerExceptions){
                        Console.WriteLine(cEx.Message);
                    }
                }
            }
        }

        private void ParseJsonConfig(ConfigInfo configInfo, DeployInfo di, Dictionary<string,string> libs)
        {
            using(var fs = File.Open(configInfo.LocalJsonConfigFile,FileMode.Open,FileAccess.Read))
            {
                string cdpath = Directory.GetCurrentDirectory();
                if(!string.IsNullOrEmpty(configInfo.CurrentWorkDirectory)){
                    Directory.SetCurrentDirectory(configInfo.CurrentWorkDirectory);
                }
                byte[] buf = new byte[fs.Length];
                fs.Read(buf,0,(int)fs.Length);
                var json = System.Text.Encoding.UTF8.GetString(buf);
                dynamic jsonRoot = JsonConvert.DeserializeObject(json);
                dynamic jsonModules = jsonRoot["modules"];
                foreach(var m in jsonModules){
                    var mi=new ModuleInfo();
                    mi.loader=new LoaderInfo();
                    mi.loader.entrypoint=new EntryPointInfo();
                    mi.args=new List<KeyValuePair<string,string>>();
                    mi.name=m["name"];
                    mi.version=m["version"];
                    dynamic loader = m["loader"];
                    mi.loader.name=loader["name"];
                    dynamic entryPoint = loader["entrypoint"];
                    var modulePath =entryPoint["module.path"];
                    var mpFI = new FileInfo(modulePath);
                    libs.Add(mi.name,mpFI.FullName);
                    mi.loader.entrypoint.modulePath= di.gateway.deployPath + Path.PathSeparator + mpFI.Name;
                    dynamic args = m["args"];
                    foreach(dynamic arg in args.Properties()){
                        dynamic argv = arg.Value;
                        mi.args.Add(new KeyValuePair<string,string>(arg.Name,argv.Value.ToString()));
                    }
                    di.modules.Add(mi);
                }
                dynamic jsonLinks =jsonRoot["links"];
                foreach(var l in jsonLinks){
                    var linkInfo = new LinkInfo();
                    linkInfo.source=l["source"];
                    linkInfo.sink=l["sink"];
                    di.links.Add(linkInfo);
                }
                if (!string.IsNullOrEmpty( configInfo.CurrentWorkDirectory)){
                    Directory.SetCurrentDirectory(cdpath);
                }
            }
        }
        private async Task Generate(ConfigInfo configInfo, DeployInfo di, Dictionary<string,string> libs)
        {
            deployInfo = di;
            connectionString=configInfo.StorageConnectionString;

            var cloudStorageAccount = CloudStorageAccount.Parse(connectionString);
            blobClient = cloudStorageAccount.CreateCloudBlobClient();
            var container = blobClient.GetContainerReference(configInfo.StorageConnectionString);
            await container.CreateIfNotExistsAsync();
            var now =DateTime.Now;
            // container should be created before this tool execution!
            foreach(var mi in di.modules){
                var lib = libs[mi.name];
                mi.loader.entrypoint.moduleUri=await UploadAndAddSAS(container,mi.name,lib,now, configInfo.DayDuration);
            }
            var json = JsonConvert.SerializeObject(di);
            using(var fs = File.Open(configInfo.ModuleConfigJsonFileName,FileMode.Create,FileAccess.Write)){
                var writer = new StreamWriter(fs);
                writer.Write(json);
                writer.Flush();
            }
            var cfgUri = await UploadAndAddSAS(container,"", configInfo.ModuleConfigJsonFileName,now,configInfo.DayDuration);
            var twinConfig=new {
                gateway =new {
                    configuration = cfgUri
                }
            };
            var twinConfigJson = JsonConvert.SerializeObject(twinConfig);
            if (string.IsNullOrEmpty(configInfo.TwinConfigJsonFileName)){
                Console.WriteLine(twinConfigJson);
            }else {
                using(var fs = File.Open(configInfo.TwinConfigJsonFileName,FileMode.Create, FileAccess.Write)){
                    var writer = new StreamWriter(fs);
                    writer.Write(twinConfigJson);
                    writer.Flush();
                }
            }
            if (!string.IsNullOrEmpty(configInfo.IoTHubConnectionString)&&!string.IsNullOrEmpty(configInfo.EdgeConfigJsonFileName)){
                string transport = "amqp";
                if (!string.IsNullOrEmpty(configInfo.IoTHubTransport)){
                    transport=configInfo.IoTHubTransport;
                }
                var edgeConfig=new{
                    gateway=new EdgeGatewayInfo(){
                        connectionString=configInfo.IoTHubConnectionString,
                        transport=transport
                    },
                    modules=new List<ModuleInfo>(),
                    links=new List<LinkInfo>()
                };
                var edgeConfigJson = JsonConvert.SerializeObject(edgeConfig);
                using(var fs = File.Open(configInfo.EdgeConfigJsonFileName,FileMode.Create,FileAccess.Write)){
                    var writer =new StreamWriter(fs);
                    writer.Write(edgeConfigJson);
                    writer.Flush();
                }
            }
        }

        private async Task<string> UploadAndAddSAS(CloudBlobContainer container, string folderName, string localFilePath, DateTime now, double duration)
        {
            var fi = new FileInfo(localFilePath);
            string blobName = fi.Name;
            if (!string.IsNullOrEmpty(folderName)){
                blobName = folderName+"/"+blobName;
            }
            var blob = container.GetBlockBlobReference(blobName);
            await blob.UploadFromFileAsync(fi.FullName);

            var sasConstraints =new SharedAccessBlobPolicy();
            sasConstraints.SharedAccessStartTime=now.AddMinutes(-1.0);
            sasConstraints.SharedAccessExpiryTime=now.AddDays(duration);
            sasConstraints.Permissions= SharedAccessBlobPermissions.Read;
            string sasToken = blob.GetSharedAccessSignature(sasConstraints);
            
            return blob.Uri+sasToken;
        }
        CloudBlobClient blobClient;
        string connectionString = "";
        DeployInfo deployInfo;

        private void UploadLibToBlob(ModuleInfo mi){

        }
    }

    public class EdgeGatewayInfo{
        [JsonProperty("connection-string")]
        public string connectionString{get;set;}
        public string transport{get;set;}
    }
    public class ConfigInfo{
        public string IoTHubConnectionString{get;set;}
        public string IoTHubTransport{get;set;}
        public string StorageConnectionString{get;set;}
        public string BlobContainerName{get;set; }
        public string ModuleConfigJsonFileName{get;set;}
        public string TwinConfigJsonFileName{get;set;}
        public string EdgeConfigJsonFileName{get;set;}
        public double DayDuration{get;set;}

        public string CurrentWorkDirectory{get;set;}
        public string LocalJsonConfigFile{get;set;}
    }

    public class GatewayInfo {
        [JsonProperty("deploy-path")]
        public string deployPath{get;set;}
        public string version{get;  set;}
    }

    public class EntryPointInfo{
        [JsonProperty("module.url")]
        public string moduleUri{get;set;}
        [JsonProperty("module.path")]
        public string modulePath{get;set;}
    }
    public class LoaderInfo{
        public string name{get;set;}
        public EntryPointInfo entrypoint{get;set;}
    }
    public class ModuleInfo{
        public string name{get;set;}
        public LoaderInfo loader{get;set;}
        public List<KeyValuePair<string, string>> args{get;set; }
        public string version{get;set;}
    }

    public class LinkInfo{
        public string source{get;set;}
        public string sink{get;set;}
    }

    public class DeployInfo{
        public GatewayInfo gateway{get;set;}
        public List<ModuleInfo> modules{get;set;}
        public List<LinkInfo> links{get;set;}
    }

    
}
